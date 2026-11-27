// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef M17_BENCH_H
#define M17_BENCH_H

#include "config.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stdbool.h>
#include <stdint.h>
#include "c2fx.h"

typedef struct {
	uint32_t dec_cycles;
	uint32_t dec_mean;
	uint32_t dec_crc;
	uint32_t audio_free;
	uint32_t prof_total;
	uint32_t probe_cycles;
	uint32_t cos_cycles;
	uint32_t cos_calls;
	uint32_t exp2_cycles;
	uint32_t exp2_calls;
	uint32_t log2_cycles;
	uint32_t log2_calls;
	uint32_t rfft_cycles;
	uint32_t rfft_calls;
	uint32_t irfft_cycles;
	uint32_t irfft_calls;
	uint32_t loop_cycles;
	uint32_t dec_nosched;
	uint32_t dec_masked;
} m17_bench_result_t;

// When set, hal_cycles() read sampling for the plain-decode loop only counts toward external tallies (host test).
extern volatile bool m17_bench_count_hal_reads;

// Retains the calibration loop's final value for the host test.
extern volatile uint32_t m17_bench_loop_sink;

// Run from the audio task with exclusive use of the lent decoder state and scratch.
// The caller supplies temporary PCM storage. False means no codec memory.
bool m17_bench_run(c2fx_dec_t *dec, c2fx_scratch_t *scratch, int16_t pcm[C2FX_FRAME_LEN],
		   m17_bench_result_t *result);
#endif
#endif