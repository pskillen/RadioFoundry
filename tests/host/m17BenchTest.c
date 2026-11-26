// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "debuglink/debuglink_m17_bench.h"
#include "debuglink/dl_frame.h"
#include "hal_time.h"
#include "m17/m17_bench.h"
#include "radio/radio.h"
#include "task.h"

static uint32_t cycles;
static unsigned reads;
static uint32_t now;
static uint8_t flags;
static bool audio_scheduled;
static c2fx_dec_t dec;
static c2fx_scratch_t *scratch;
static int16_t pcm[C2FX_FRAME_LEN];

uint32_t hal_cycles(void)
{
	cycles += ++reads == 64 ? 5 : 1;
	return cycles;
}

uint32_t hal_millis(void)
{
	return now;
}

void hal_delay_ms(uint32_t ms)
{
	now += ms;
	if (audio_scheduled) {
		audio_scheduled = false;
		debuglink_m17_bench_audio(&dec, scratch, pcm);
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
	char buf[256];
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
	if (debuglink_m17_bench_run() != DL_ST_OK || reads != 100)
		return false;
	dl_text_init(&text, buf, sizeof buf);
	debuglink_m17_bench_view(&text);
	return !text.full && strcmp(buf, "c2.dec_cycles=5\nc2.dec_crc=0xb012ff9e\nc2.audio_free=68\n"
					"c2.scratch=4504\nc2.dec_bytes=412\nc2.enc=0\n") == 0;
}

int main(void)
{
	scratch = malloc(sizeof *scratch);
	if (scratch == NULL)
		return 1;
	bool passed = measurements_match() && command_completes();
	free(scratch);
	if (!passed) {
		fputs("m17BenchTest: bench check failed\n", stderr);
		return 1;
	}
	puts("m17BenchTest: PCM CRC b012ff9e, 50 timed decodes, missing scratch refused, command and view passed");
	return 0;
}
