// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef M17_BENCH_H
#define M17_BENCH_H

#include "config.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stdbool.h>
#include "c2fx.h"

typedef struct {
	uint32_t dec_cycles;
	uint32_t dec_crc;
	uint32_t audio_free;
} m17_bench_result_t;

// Run from the audio task with exclusive use of the lent decoder state and scratch.
// The caller supplies temporary PCM storage. False means no codec memory.
bool m17_bench_run(c2fx_dec_t *dec, c2fx_scratch_t *scratch, int16_t pcm[C2FX_FRAME_LEN],
		   m17_bench_result_t *result);
#endif
#endif
