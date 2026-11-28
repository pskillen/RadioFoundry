// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
/*
 * M17 voice between the engine (m17_engine.h) and the board's PCM audio (hal_audio_t).
 * docs/spec/m17.md 9. Receive only: the model is core/p25/p25Voice.h.
 *
 *  RX: rx_voice_fec queues each stream frame's two codec frames. The audio block callback takes one
 *      per 20 ms and decodes it. Playback starts once four codec frames are queued (two stream frames)
 *      and one more block has passed. A frame whose Viterbi count is over 12, a frame placed by
 *      counting, and a block with nothing queued during a call go to c2fx as lost (NULL bitstream).
 *      The codec repeats, fades, then mutes. This file does not fade again.
 *
 * Threading: Begin, Poll, Halt and End run in the radio task, the hooks in the provider's task, the
 * decoder only in the audio callback. The queue is single producer / single consumer. No RTOS call.
 *
 * Memory is the caller's: codec scratch, decoder state and the queue (m17_voice_queue_t).
 */
#ifndef RFOUNDRY_M17_VOICE_H
#define RFOUNDRY_M17_VOICE_H

#include "config.h"

#if CONFIG_M17 && CONFIG_M17_VOICE

#include <stdbool.h>
#include <stdint.h>
#include "hal_audio.h"
#include "m17_engine.h"

#define M17_VOICE_QUEUE		16	// codec frames (8 stream frames), power of two
#define M17_VOICE_PRIME		4	// queued before playback: two stream frames
#define M17_VOICE_LEAD		1	// silent block after that
#define M17_VOICE_ERR_MAX	12	// Viterbi corrections at or under this are decoded

struct m17_rx;

typedef struct {
	uint8_t seq;
	uint8_t lost;		// 1: c2fx_decode with a NULL bitstream
	uint8_t bits[8];
} m17_voice_entry_t;

// The caller's queue. Counters live here, not in BSS, so a session restart drops them with the lend.
typedef struct {
	m17_voice_entry_t q[M17_VOICE_QUEUE];
	uint32_t rx_frames;	// codec frames decoded and played
	uint32_t rx_lost;	// codec frames given to the decoder as lost
	uint32_t rx_underruns;	// blocks with nothing queued during a call
} m17_voice_queue_t;

typedef struct {
	uint32_t rx_frames;
	uint32_t rx_lost;
	uint32_t rx_underruns;
} m17_voice_counts_t;

// Set the board's PCM audio interface once at startup
void m17VoiceInit(const hal_audio_t *audio);
// Radio task, before the air session can deliver frames. A NULL or misaligned pointer leaves the path idle
void m17VoiceBegin(void *scratch, void *state, void *queues, const struct m17_rx *rx);
// Radio task, every tick: starts the stream once a playable frame is queued
void m17VoicePoll(void);
// Radio task. End the old call on a retune; the consumer discards frames from the old channel
void m17VoiceHalt(void);
// Radio task, after the air session is closed: the stream stops, then the caller's memory is unused
void m17VoiceEnd(void);
const m17_engine_hooks_t *m17VoiceHooks(void);
void m17VoiceGetCounts(m17_voice_counts_t *out);

// The codec bench and a live call share decoder state. False: a call holds it, so the bench must not enter
bool m17VoiceBenchEnter(void);
void m17VoiceBenchLeave(void);

// The audio block callback (exposed for tests)
void m17VoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx);

#endif
#endif
