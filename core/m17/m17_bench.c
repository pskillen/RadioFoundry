// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "m17_bench.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stddef.h>
#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "mbe/c2fx_port.h"
#include "hal_time.h"

volatile bool m17_bench_count_hal_reads;
volatile uint32_t m17_bench_loop_sink;

// Frame 0 of the pinned c2fx 3200 chirp vector used by c2fxSeamTest.
static const uint8_t frame[C2FX_BITS_BYTES] = { 0xc8, 0xa0, 0x61, 0x72, 0x94, 0xac, 0xe7, 0x19 };

enum { PLAIN_DECODES = 50, PROFILE_DECODES = 10, PROBE_TRIES = 100 };

static uint32_t pcm_crc32(const int16_t pcm[C2FX_FRAME_LEN])
{
	uint32_t crc = UINT32_MAX;

	for (unsigned i = 0; i < C2FX_FRAME_LEN; i++) {
		uint16_t sample = (uint16_t)pcm[i];

		for (unsigned shift = 0; shift < 16; shift += 8) {
			crc ^= (sample >> shift) & 0xffu;
			for (unsigned bit = 0; bit < 8; bit++)
				crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
		}
	}
	return ~crc;
}

static uint32_t measure_probe_cost(void)
{
	uint32_t min = UINT32_MAX;

	for (unsigned i = 0; i < PROBE_TRIES; i++) {
		uint32_t start = hal_cycles();
		uint32_t cost = hal_cycles() - start;

		if (cost < min)
			min = cost;
	}
	return min == UINT32_MAX ? 0 : min;
}

static __attribute__((noinline)) uint32_t measure_loop_cycles(void)
{
	uint32_t x = 0;
	uint32_t start = hal_cycles();

	for (uint32_t i = 0; i < 1000000u; i++)
		x = x * 3u + i;
	uint32_t cycles = hal_cycles() - start;
	m17_bench_loop_sink = x;
	return cycles;
}

static void store_hook_means(m17_bench_result_t *result)
{
	result->cos_cycles = c2fx_port_cycles[C2FX_HOOK_COS] / PROFILE_DECODES;
	result->cos_calls = c2fx_port_calls[C2FX_HOOK_COS] / PROFILE_DECODES;
	result->exp2_cycles = c2fx_port_cycles[C2FX_HOOK_EXP2] / PROFILE_DECODES;
	result->exp2_calls = c2fx_port_calls[C2FX_HOOK_EXP2] / PROFILE_DECODES;
	result->log2_cycles = c2fx_port_cycles[C2FX_HOOK_LOG2] / PROFILE_DECODES;
	result->log2_calls = c2fx_port_calls[C2FX_HOOK_LOG2] / PROFILE_DECODES;
	result->rfft_cycles = c2fx_port_cycles[C2FX_HOOK_RFFT] / PROFILE_DECODES;
	result->rfft_calls = c2fx_port_calls[C2FX_HOOK_RFFT] / PROFILE_DECODES;
	result->irfft_cycles = c2fx_port_cycles[C2FX_HOOK_IRFFT] / PROFILE_DECODES;
	result->irfft_calls = c2fx_port_calls[C2FX_HOOK_IRFFT] / PROFILE_DECODES;
}

bool m17_bench_run(c2fx_dec_t *dec, c2fx_scratch_t *scratch, int16_t pcm[C2FX_FRAME_LEN],
		   m17_bench_result_t *result)
{
	*result = (m17_bench_result_t){ 0 };
	if (dec == NULL || scratch == NULL)
		return false;

	c2fx_dec_init(dec);
	c2fx_port_profile = false;

	uint64_t dec_sum = 0;

	m17_bench_count_hal_reads = true;
	for (unsigned i = 0; i < PLAIN_DECODES; i++) {
		uint32_t start = hal_cycles();

		c2fx_decode(dec, frame, pcm, scratch);
		uint32_t cycles = hal_cycles() - start;
		dec_sum += cycles;
		if (cycles > result->dec_cycles)
			result->dec_cycles = cycles;
		if (i == 0)
			result->dec_crc = pcm_crc32(pcm);
	}
	m17_bench_count_hal_reads = false;
	result->dec_mean = (uint32_t)(dec_sum / PLAIN_DECODES);

	result->probe_cycles = measure_probe_cost();

	c2fx_port_profile_reset();
	c2fx_port_profile = true;
	uint64_t prof_sum = 0;
	for (unsigned i = 0; i < PROFILE_DECODES; i++) {
		uint32_t start = hal_cycles();

		c2fx_decode(dec, frame, pcm, scratch);
		prof_sum += hal_cycles() - start;
	}
	c2fx_port_profile = false;

	result->prof_total = (uint32_t)(prof_sum / PROFILE_DECODES);
	store_hook_means(result);

	result->loop_cycles = measure_loop_cycles();

	// Reset the decoder history before each probe to match the first plain decode.
	c2fx_dec_init(dec);
	vTaskSuspendAll();
	uint32_t start = hal_cycles();
	c2fx_decode(dec, frame, pcm, scratch);
	uint32_t cycles = hal_cycles() - start;
	xTaskResumeAll();
	result->dec_nosched = cycles;

	// Mask interrupts for one decode only to bound latency and stay within the audio hang check.
	c2fx_dec_init(dec);
	taskENTER_CRITICAL();
	start = hal_cycles();
	c2fx_decode(dec, frame, pcm, scratch);
	cycles = hal_cycles() - start;
	taskEXIT_CRITICAL();
	result->dec_masked = cycles;

	result->audio_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
	return true;
}
#endif