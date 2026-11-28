// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 voice, receive half: stream frames to c2fx to PCM blocks (m17Voice.h)
#include "config.h"
#if CONFIG_M17 && CONFIG_M17_VOICE	// the whole file
#include "m17Voice.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "c2fx.h"
#include "m17_rx.h"

#if (M17_VOICE_QUEUE & (M17_VOICE_QUEUE - 1)) != 0
#error "M17_VOICE_QUEUE must be a power of two"
#endif
_Static_assert(sizeof(m17_voice_queue_t) <= 512, "voice queue exceeds the reserved 512 bytes");
_Static_assert(C2FX_BITS_BYTES == 8, "a codec frame is 8 bytes");
_Static_assert(AUDIO_BLOCK_SAMPLES == C2FX_FRAME_LEN, "one PCM block is one codec frame");

#define LOAD(x)		__atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define STORE(x, v)	__atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define MASK		(M17_VOICE_QUEUE - 1)

enum { CODEC_FREE = 0, CODEC_VOICE = 1, CODEC_BENCH = 2 };

static const hal_audio_t *aud;
static bool streaming;			// radio task only
static void *scratch;
static c2fx_dec_t *dec;
static m17_voice_queue_t *qmem;
static const m17_rx_t *rx_bound;

// Producer: the provider task (head). Consumer: the audio callback (tail)
static uint8_t rx_head, rx_tail;
static uint8_t rx_seq, rx_end_seq;
// Held for a whole call. The bench's c2fx_dec_init would wipe a live decoder if it ran between blocks
static uint8_t codec_lock;

static uint8_t cb_seq;
static bool cb_live, cb_primed, cb_speaker;
static bool cb_speaker_unsent;
static uint8_t cb_lead;

// Load and store only. C-SKY has no libatomic, and the bench and the callback share the audio task,
// so they never run at once. The radio task only releases.
static bool codec_claim(uint8_t who)
{
	if (LOAD(codec_lock) != CODEC_FREE)
		return false;
	STORE(codec_lock, who);
	return true;
}

static void codec_release(uint8_t who)
{
	if (LOAD(codec_lock) == who)
		STORE(codec_lock, CODEC_FREE);
}

// True when this call may use the decoder. A bench holding it yields this block
static bool voice_codec(void)
{
	uint8_t cur = __atomic_load_n(&codec_lock, __ATOMIC_ACQUIRE);

	if (cur == CODEC_BENCH)
		return false;
	if (cur == CODEC_VOICE)
		return true;
	return codec_claim(CODEC_VOICE);
}

static bool plain_voice(const m17_rx_t *rx)
{
	uint16_t type;

	if (rx == NULL || !rx->type_known)
		return false;
	type = m17_rx_type(rx);
	return (type & M17_TYPE_STREAM) != 0 && (type & M17_TYPE_DATA_MASK) == M17_TYPE_VOICE &&
	       (type & M17_TYPE_ENC_MASK) == 0;
}

// Bit 15 marks the last voice frame. The signature frames are the low 15 bits, 0x7FFB through 0x7FFF
static bool signature_fn(uint16_t fn)
{
	return (uint16_t)(fn & 0x7fffu) >= 0x7ffbu;
}

static void queue_one(uint8_t seq, bool lost, const uint8_t *bits)
{
	m17_voice_queue_t *q = LOAD(qmem);
	uint8_t h;

	if (q == NULL)
		return;
	h = rx_head;
	if ((uint8_t)(h - LOAD(rx_tail)) >= M17_VOICE_QUEUE)
		return;
	m17_voice_entry_t *e = &q->q[h & MASK];
	e->seq = seq;
	e->lost = lost ? 1 : 0;
	if (!lost)
		memcpy(e->bits, bits, 8);
	STORE(rx_head, (uint8_t)(h + 1));
}

static void open_call_if_idle(void)
{
	uint8_t seq = LOAD(rx_seq);

	if (LOAD(rx_end_seq) != seq)
		return;
	STORE(rx_seq, (uint8_t)(seq + 1));
}

static void rx_start(const struct m17_rx *rx, void *ctx)
{
	(void)rx;
	(void)ctx;
	if (LOAD(qmem) == NULL)
		return;
	STORE(rx_seq, (uint8_t)(LOAD(rx_seq) + 1));
}

static void rx_end(void *ctx)
{
	(void)ctx;
	STORE(rx_end_seq, LOAD(rx_seq));
}

static void rx_lost(void *ctx)
{
	uint8_t seq;

	(void)ctx;
	if (LOAD(qmem) == NULL || !plain_voice(LOAD(rx_bound)))
		return;
	open_call_if_idle();
	seq = LOAD(rx_seq);
	queue_one(seq, true, NULL);
	queue_one(seq, true, NULL);
}

static void rx_voice_fec(const uint8_t *voice, uint16_t fn, int errs, void *ctx)
{
	uint8_t seq;
	bool lost;

	(void)ctx;
	if (LOAD(qmem) == NULL || !plain_voice(LOAD(rx_bound)))
		return;
	// A failed decode has no frame number and no payload
	if (errs < 0)
		return;
	if (signature_fn(fn))
		return;
	open_call_if_idle();
	seq = LOAD(rx_seq);
	lost = errs > M17_VOICE_ERR_MAX;
	queue_one(seq, lost, lost ? NULL : voice);
	queue_one(seq, lost, lost ? NULL : voice + 8);
}

static const m17_engine_hooks_t hooks = {
	.rx_start = rx_start,
	.rx_end = rx_end,
	.rx_lost = rx_lost,
	.rx_voice_fec = rx_voice_fec,
};

static void speaker(bool on)
{
	if (cb_speaker != on || LOAD(cb_speaker_unsent)) {
		cb_speaker = on;
		STORE(cb_speaker_unsent, false);
		if (aud != NULL && aud->speaker_pcm != NULL)
			aud->speaker_pcm(on);
	}
}

static void silence(int16_t *spk)
{
	memset(spk, 0, AUDIO_BLOCK_SAMPLES * sizeof(int16_t));
}

static void count_add(uint32_t *field)
{
	// One writer, the audio callback. A fetch-add would pull in libatomic on C-SKY
	__atomic_store_n(field, __atomic_load_n(field, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
}

static void rx_block(int16_t *spk)
{
	m17_voice_queue_t *q = LOAD(qmem);
	uint8_t seq, t, h, queued;
	bool ended;

	if (q == NULL) {
		silence(spk);
		return;
	}
	// Read the head first so frames from a newer call are never discarded as old.
	h = LOAD(rx_head);
	seq = LOAD(rx_seq);
	if (seq != cb_seq) {
		if (!voice_codec()) {
			silence(spk);
			return;
		}
		cb_seq = seq;
		cb_live = true;
		cb_primed = false;
		cb_lead = 0;
		c2fx_dec_init(dec);
	}
	t = rx_tail;
	while (t != h && q->q[t & MASK].seq != cb_seq)
		t++;
	STORE(rx_tail, t);
	ended = LOAD(rx_end_seq) == cb_seq;
	queued = (uint8_t)(h - t);
	if (!cb_live) {
		silence(spk);
		return;
	}
	if (!voice_codec()) {
		silence(spk);
		return;
	}
	if (!cb_primed) {
		if (queued < M17_VOICE_PRIME) {
			speaker(false);
			if (ended) {
				cb_live = false;
				STORE(rx_tail, h);
				codec_release(CODEC_VOICE);
			}
			silence(spk);
			return;
		}
		cb_primed = true;
		cb_lead = M17_VOICE_LEAD;
		speaker(true);
	}
	if (cb_lead != 0) {
		cb_lead--;
		silence(spk);
		return;
	}
	if (queued == 0) {
		if (ended) {
			cb_live = false;
			speaker(false);
			codec_release(CODEC_VOICE);
			silence(spk);
			return;
		}
		count_add(&q->rx_underruns);
		c2fx_decode(dec, NULL, spk, scratch);
		return;
	}
	{
		const m17_voice_entry_t *e = &q->q[t & MASK];
		bool lost = e->lost != 0;

		if (lost)
			c2fx_decode(dec, NULL, spk, scratch);
		else
			c2fx_decode(dec, e->bits, spk, scratch);
		STORE(rx_tail, (uint8_t)(t + 1));
		if (lost)
			count_add(&q->rx_lost);
		else
			count_add(&q->rx_frames);
	}
}

void m17VoiceAudioBlock(const int16_t *mic, int16_t *spk, void *ctx)
{
	(void)mic;
	(void)ctx;
	if (LOAD(qmem) == NULL) {
		silence(spk);
		return;
	}
	rx_block(spk);
	if (LOAD(cb_speaker_unsent))
		speaker(cb_speaker);
}

static void stream_start(void)
{
	if (aud == NULL || aud->start == NULL)
		return;
	STORE(cb_speaker_unsent, true);
	streaming = aud->start(m17VoiceAudioBlock, NULL);
}

void m17VoiceInit(const hal_audio_t *audio)
{
	aud = audio;
}

void m17VoiceBegin(void *sc, void *st, void *qu, const struct m17_rx *rx)
{
	rx_head = rx_tail = 0;
	rx_seq = rx_end_seq = 0;
	cb_seq = 0;
	cb_live = cb_primed = cb_speaker = false;
	cb_lead = 0;
	STORE(cb_speaker_unsent, false);
	if (sc == NULL || st == NULL || qu == NULL ||
	    (((uintptr_t)sc | (uintptr_t)st | (uintptr_t)qu) & 7u) != 0) {
		scratch = NULL;
		dec = NULL;
		STORE(rx_bound, NULL);
		STORE(qmem, NULL);
		return;
	}
	scratch = sc;
	dec = st;
	memset(qu, 0, sizeof(m17_voice_queue_t));
	// The callback initializes the decoder on the first block of the call. Begin stays off the audio task
	STORE(rx_bound, rx);
	STORE(qmem, qu);
}

void m17VoicePoll(void)
{
	if (!streaming && aud != NULL && aud->start != NULL && LOAD(qmem) != NULL &&
	    LOAD(rx_head) != LOAD(rx_tail))
		stream_start();
}

void m17VoiceHalt(void)
{
	uint8_t seq;

	if (LOAD(qmem) == NULL)
		return;
	seq = (uint8_t)(LOAD(rx_seq) + 1);
	STORE(rx_seq, seq);
	STORE(rx_end_seq, seq);
}

void m17VoiceEnd(void)
{
	if (streaming && aud != NULL && aud->stop != NULL) {
		aud->stop();
		if (aud->speaker_pcm != NULL)
			aud->speaker_pcm(false);
	}
	streaming = false;
	cb_speaker = false;
	codec_release(CODEC_VOICE);
	STORE(rx_bound, NULL);
	scratch = NULL;
	dec = NULL;
	STORE(qmem, NULL);
}

const m17_engine_hooks_t *m17VoiceHooks(void)
{
	return &hooks;
}

void m17VoiceGetCounts(m17_voice_counts_t *out)
{
	m17_voice_queue_t *q = LOAD(qmem);

	if (q == NULL) {
		memset(out, 0, sizeof *out);
		return;
	}
	out->rx_frames = __atomic_load_n(&q->rx_frames, __ATOMIC_RELAXED);
	out->rx_lost = __atomic_load_n(&q->rx_lost, __ATOMIC_RELAXED);
	out->rx_underruns = __atomic_load_n(&q->rx_underruns, __ATOMIC_RELAXED);
}

bool m17VoiceBenchEnter(void)
{
	return codec_claim(CODEC_BENCH);
}

void m17VoiceBenchLeave(void)
{
	codec_release(CODEC_BENCH);
}

#endif
