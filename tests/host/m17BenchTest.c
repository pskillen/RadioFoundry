// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app/audio_job.h"
#include "c2fx_port.h"
#include "debuglink/debuglink_m17_bench.h"
#include "debuglink/dl_frame.h"
#include "hal_time.h"
#include "m17/m17_bench.h"
#include "radio/radio.h"
#include "task.h"

static const char EXPECTED_VIEW_PREFIX[] =
	"c2.dec_cycles=5\nc2.dec_crc=0xb012ff9e\nc2.audio_free=68\n"
	"c2.scratch=4504\nc2.dec_bytes=412\nc2.enc=0\n";

static const char *const PROFILE_KEYS[] = {
	"c2.dec_mean=", "c2.prof_total=", "c2.probe_cycles=", "c2.cos_cycles=", "c2.cos_calls=",
	"c2.exp2_cycles=", "c2.exp2_calls=", "c2.log2_cycles=", "c2.log2_calls=", "c2.rfft_cycles=",
	"c2.rfft_calls=", "c2.irfft_cycles=", "c2.irfft_calls=",
};

static uint32_t cycles;
static unsigned reads;
static uint32_t now;
static uint8_t flags;
static bool audio_scheduled;
static unsigned audio_wakes, audio_jobs;
static c2fx_dec_t dec;
static c2fx_scratch_t *scratch;
static int16_t pcm[C2FX_FRAME_LEN];

uint32_t hal_cycles(void)
{
	if (m17_bench_count_hal_reads)
		cycles += ++reads == 64 ? 5 : 1;
	else
		cycles++;
	return cycles;
}

uint32_t hal_millis(void)
{
	return now;
}

static void bench_audio_job(void)
{
	audio_jobs++;
	debuglink_m17_bench_audio(&dec, scratch, pcm);
}

void xTaskNotifyGive(TaskHandle_t task)
{
	audio_wakes++;
}

void hal_delay_ms(uint32_t ms)
{
	now += ms;
	if (audio_scheduled) {
		audio_scheduled = false;
		if (!debuglink_m17_bench_armed() || !audio_job_submit(NULL, bench_audio_job) ||
		    audio_job_submit(NULL, bench_audio_job))
			abort();
		audio_job_service();
		audio_job_service();	// the pending job was consumed
	}
}

unsigned uxTaskGetStackHighWaterMark(void *task)
{
	return 17;
}

void radio_get_view(radio_view_t *view)
{
	*view = (radio_view_t){ .m17.flags = flags };
}

static const char *view_line(const char *view, const char *key)
{
	const char *line = strstr(view, key);

	if (line == NULL)
		return NULL;
	line += strlen(key);
	return line;
}

static bool view_has_keys(const char *view)
{
	for (size_t i = 0; i < sizeof(PROFILE_KEYS) / sizeof(PROFILE_KEYS[0]); i++) {
		if (strstr(view, PROFILE_KEYS[i]) == NULL)
			return false;
	}
	return true;
}

static bool measurements_match(void)
{
	m17_bench_result_t result;

	if (!m17_bench_run(&dec, scratch, pcm, &result) || result.dec_crc != 0xb012ff9eu ||
	    result.dec_cycles != 5 || result.audio_free != 68 || reads != 100) {
		fprintf(stderr, "m17BenchTest: PCM CRC %08" PRIx32 ", max cycles %" PRIu32 ", reads %u\n",
			result.dec_crc, result.dec_cycles, reads);
		return false;
	}
	if (m17_bench_run(&dec, NULL, pcm, &result) || result.dec_crc != 0 || result.dec_cycles != 0 ||
	    result.audio_free != 0 || reads != 100) {
		fprintf(stderr, "m17BenchTest: missing scratch was not refused\n");
		return false;
	}
	return true;
}

static bool command_completes(void)
{
	char buf[512];
	dl_text_t text;

	if (debuglink_m17_bench_run() != DL_ST_REFUSED)
		return false;
	flags = RADIO_M17_ON;
	if (debuglink_m17_bench_run() != DL_ST_UNAVAILABLE)
		return false;
	flags |= RADIO_M17_VOICE_MEM;
	if (debuglink_m17_bench_run() != DL_ST_REFUSED || debuglink_m17_bench_audio(&dec, scratch, pcm))
		return false;

	audio_scheduled = true;
	reads = 0;
	if (debuglink_m17_bench_run() != DL_ST_OK || reads != 100 || audio_wakes != 1 || audio_jobs != 1)
		return false;
	dl_text_init(&text, buf, sizeof buf);
	debuglink_m17_bench_view(&text);
	return !text.full && strncmp(buf, EXPECTED_VIEW_PREFIX, sizeof(EXPECTED_VIEW_PREFIX) - 1) == 0;
}

static bool profile_checks(void)
{
	char buf[512];
	dl_text_t text;
	m17_bench_result_t result;
	uint32_t calls[C2FX_HOOK_COUNT];
	uint32_t hook_sum;

	dl_text_init(&text, buf, sizeof buf);
	debuglink_m17_bench_view(&text);
	if (text.full || !view_has_keys(buf))
		return false;

	if (!m17_bench_run(&dec, scratch, pcm, &result))
		return false;
	if (result.rfft_calls == 0 || result.cos_calls == 0)
		return false;
	hook_sum = result.cos_cycles + result.exp2_cycles + result.log2_cycles + result.rfft_cycles +
		   result.irfft_cycles;
	if (hook_sum > result.prof_total)
		return false;
	if (c2fx_port_profile)
		return false;

	uint32_t hook_cycles[C2FX_HOOK_COUNT];

	for (unsigned i = 0; i < C2FX_HOOK_COUNT; i++) {
		calls[i] = c2fx_port_calls[i];
		hook_cycles[i] = c2fx_port_cycles[i];
	}
	c2fx_decode(&dec, (const uint8_t[]){ 0xc8, 0xa0, 0x61, 0x72, 0x94, 0xac, 0xe7, 0x19 }, pcm, scratch);
	for (unsigned i = 0; i < C2FX_HOOK_COUNT; i++) {
		if (c2fx_port_calls[i] != calls[i] || c2fx_port_cycles[i] != hook_cycles[i])
			return false;
	}
	const char *rfft_calls = view_line(buf, "c2.rfft_calls=");
	const char *cos_calls = view_line(buf, "c2.cos_calls=");
	const char *prof_total = view_line(buf, "c2.prof_total=");

	if (rfft_calls == NULL || cos_calls == NULL || prof_total == NULL)
		return false;
	if (strtoul(rfft_calls, NULL, 10) == 0 || strtoul(cos_calls, NULL, 10) == 0)
		return false;
	return strtoul(prof_total, NULL, 10) >= hook_sum;
}

int main(void)
{
	scratch = malloc(sizeof *scratch);
	if (scratch == NULL)
		return 1;
	reads = 0;
	bool passed = measurements_match() && command_completes() && profile_checks();
	free(scratch);
	if (!passed) {
		fputs("m17BenchTest: bench check failed\n", stderr);
		return 1;
	}
	puts("m17BenchTest: PCM CRC b012ff9e, 50 timed decodes, missing scratch refused, command and view passed");
	return 0;
}