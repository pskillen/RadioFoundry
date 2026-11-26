// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "m17_bench.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hal_time.h"

// Frame 0 of the pinned c2fx 3200 chirp vector used by c2fxSeamTest.
static const uint8_t frame[C2FX_BITS_BYTES] = { 0xc8, 0xa0, 0x61, 0x72, 0x94, 0xac, 0xe7, 0x19 };

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

bool m17_bench_run(c2fx_dec_t *dec, c2fx_scratch_t *scratch, int16_t pcm[C2FX_FRAME_LEN],
		   m17_bench_result_t *result)
{
	*result = (m17_bench_result_t){ 0 };
	if (dec == NULL || scratch == NULL)
		return false;

	c2fx_dec_init(dec);
	for (unsigned i = 0; i < 50; i++) {
		uint32_t start = hal_cycles();

		c2fx_decode(dec, frame, pcm, scratch);
		uint32_t cycles = hal_cycles() - start;
		if (cycles > result->dec_cycles)
			result->dec_cycles = cycles;
		if (i == 0)
			result->dec_crc = pcm_crc32(pcm);
	}
	result->audio_free = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
	return true;
}
#endif
