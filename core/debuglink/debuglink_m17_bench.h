// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef DEBUGLINK_M17_BENCH_H
#define DEBUGLINK_M17_BENCH_H

#include "config.h"
#if CONFIG_DEBUGLINK && CONFIG_M17_VOICE
#include "debuglink/dl_fmt.h"
#include "hal_audio.h"

uint8_t debuglink_m17_bench_run(void);
void debuglink_m17_bench_view(dl_text_t *t);

// Call on the audio task while the active session owns these regions.
// PCM is temporary storage and must be discarded after the call returns true.
bool debuglink_m17_bench_audio(void *dec, void *scratch, int16_t pcm[AUDIO_BLOCK_SAMPLES]);
#endif
#endif
