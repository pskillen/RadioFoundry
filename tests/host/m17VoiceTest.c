// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 voice receive (core/m17/m17Voice.c) with a fake decoder. Two layouts, then the queue rules:
// four codec frames and one silent block before samples, a count of 12 versus 13, a counted gap,
// signature frame numbers, TYPE filtering, a failed decode, end of transmission, and a restart.
#include <stdio.h>
#include <string.h>

#include "c2fx.h"
#include "m17/m17Voice.h"
#include "m17/m17_rx.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static bool running, spk_on;
static int starts, stops;

static bool fake_start(audio_block_cb_t cb, void *ctx)
{
	(void)cb;
	(void)ctx;
	running = true;
	starts++;
	return true;
}
static void fake_stop(void) { running = false; stops++; }
static void fake_speaker(bool on) { spk_on = on; }
static const hal_audio_t audio = { .start = fake_start, .stop = fake_stop, .speaker_pcm = fake_speaker };

static int inits, decodes, nulls, null_run, nplayed;
static uint8_t played[64];

void c2fx_dec_init(c2fx_dec_t *d)
{
	(void)d;
	inits++;
	null_run = 0;
}

// A lost frame fades 40, 20, then 0. A real frame copies its first byte into every sample
void c2fx_decode(c2fx_dec_t *d, const uint8_t *bits, int16_t pcm[C2FX_FRAME_LEN], c2fx_scratch_t *scratch)
{
	int16_t level;

	(void)d;
	(void)scratch;
	decodes++;
	if (bits == NULL) {
		nulls++;
		level = null_run == 0 ? 40 : null_run == 1 ? 20 : 0;
		null_run++;
	} else {
		null_run = 0;
		level = bits[0] == 0 ? 1 : (int16_t)bits[0];
		if (nplayed < (int)sizeof played)
			played[nplayed++] = bits[0];
	}
	for (int i = 0; i < C2FX_FRAME_LEN; i++)
		pcm[i] = level;
}

static m17_rx_t rx;
static const m17_engine_hooks_t *hk;
static int16_t spk[AUDIO_BLOCK_SAMPLES];

static void set_type(uint16_t type)
{
	rx.type_known = true;
	rx.lsf[M17_LSF_TYPE_OFFSET] = (uint8_t)(type >> 8);
	rx.lsf[M17_LSF_TYPE_OFFSET + 1] = (uint8_t)type;
}

static void begin_at(void *scratch, void *state, void *queues)
{
	m17VoiceEnd();
	running = false;
	spk_on = false;
	starts = stops = 0;
	nplayed = decodes = nulls = 0;
	set_type(M17_TYPE_STREAM | M17_TYPE_VOICE);
	m17VoiceBegin(scratch, state, queues, &rx);
}

static bool block(void)
{
	int16_t mic[AUDIO_BLOCK_SAMPLES] = { 0 };
	bool any = false;

	memset(spk, 0, sizeof spk);
	m17VoiceAudioBlock(mic, spk, NULL);
	for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++)
		if (spk[i] != 0)
			any = true;
	return any;
}

static void feed(uint16_t fn, int errs, uint8_t a, uint8_t b)
{
	uint8_t voice[16];

	memset(voice, 0, sizeof voice);
	voice[0] = a;
	voice[8] = b;
	if (errs < 0)
		hk->rx_voice_fec(NULL, 0, errs, NULL);
	else
		hk->rx_voice_fec(voice, fn, errs, NULL);
}

// Two stream frames, the silent lead block, then the first sample. Returns the played byte
static int play_one(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
	int before = nplayed;

	hk->rx_start(&rx, NULL);
	feed(1, 0, a, b);
	feed(2, 0, c, d);
	m17VoicePoll();
	CHECK(running && starts == 1, "stream did not start once frames were queued");
	CHECK(!block() && spk_on, "the lead block was not silent with the speaker on");
	CHECK(block() && spk[0] == (int16_t)a && spk[AUDIO_BLOCK_SAMPLES - 1] == (int16_t)a,
		"first codec frame did not fill the block");
	CHECK(nplayed == before + 1 && played[before] == a, "played %u", played[before]);
	return before;
}

static void test_playback(void *scratch, void *state, void *queues, const char *layout)
{
	int base;

	begin_at(scratch, state, queues);
	m17VoicePoll();
	CHECK(!running && starts == 0, "%s: stream started with an empty queue", layout);
	base = play_one(0x11, 0x22, 0x33, 0x44);
	CHECK(block() && played[base + 1] == 0x22, "%s: second codec frame", layout);
	CHECK(block() && played[base + 2] == 0x33, "%s: third", layout);
	CHECK(block() && played[base + 3] == 0x44, "%s: fourth", layout);

	m17VoiceEnd();
	CHECK(!running && stops == 1 && !spk_on, "%s: end left the stream running", layout);
	memset(queues, 0xa5, sizeof(m17_voice_queue_t));
	{
		int d = decodes;
		CHECK(!block() && decodes == d, "%s: a block after end used the returned memory", layout);
	}
	begin_at(scratch, state, queues);
	base = play_one(0x51, 0x52, 0x53, 0x54);
	CHECK(played[base] == 0x51, "%s: the restarted call played the old queue", layout);
	m17VoiceEnd();
}

static void drain(int n)
{
	for (int i = 0; i < n; i++)
		block();
}

static void test_rules(void *scratch, void *state, void *queues)
{
	m17_voice_counts_t c;
	int nulls_before, decodes_before;

	begin_at(scratch, state, queues);
	CHECK(hk->rx_packet == NULL, "voice hooks carry a packet callback");
	play_one(0x11, 0x12, 0x13, 0x14);
	drain(3);
	m17VoiceGetCounts(&c);
	CHECK(c.rx_frames == 4 && c.rx_lost == 0, "frames %u lost %u", (unsigned)c.rx_frames, (unsigned)c.rx_lost);

	feed(3, 12, 0x21, 0x22);
	CHECK(block() && played[nplayed - 1] == 0x21 && nulls == 0, "a count of 12 was treated as lost");
	CHECK(block() && played[nplayed - 1] == 0x22, "the second half of a count of 12");

	nulls_before = nulls;
	feed(4, 13, 0x31, 0x32);
	CHECK(block() && spk[0] == 40 && spk[AUDIO_BLOCK_SAMPLES - 1] == 40, "count 13 was not a NULL decode");
	CHECK(block() && spk[0] == 20, "the codec fade was rewritten");
	m17VoiceGetCounts(&c);
	CHECK(nulls == nulls_before + 2 && c.rx_lost == 2, "lost %d counts %u", nulls - nulls_before,
		(unsigned)c.rx_lost);

	nulls_before = nulls;
	hk->rx_lost(NULL);
	block();
	block();
	m17VoiceGetCounts(&c);
	CHECK(nulls == nulls_before + 2 && c.rx_lost == 4, "gap nulls %d lost %u", nulls - nulls_before,
		(unsigned)c.rx_lost);

	nulls_before = nulls;
	decodes_before = decodes;
	block();
	CHECK(nulls == nulls_before + 1, "an empty queue during a call was not a lost frame");
	m17VoiceGetCounts(&c);
	CHECK(c.rx_underruns == 1, "underruns %u", (unsigned)c.rx_underruns);
	CHECK(decodes == decodes_before + 1, "the underrun did not call the decoder");

	hk->rx_end(NULL);
	nulls_before = nulls;
	CHECK(!block() && nulls == nulls_before && !spk_on, "the ended call kept concealing");
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	feed(0x7ffb, 0, 0xab, 0xcd);
	feed(0xffff, 0, 0xab, 0xcd);
	feed(0x7fff, 0, 0xab, 0xcd);
	m17VoicePoll();
	CHECK(starts == 0 && decodes == 0, "a signature frame was queued");
	play_one(0x61, 0x62, 0x63, 0x64);
	// 0x8001 keeps bit 15 and is still a voice frame
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	hk->rx_start(&rx, NULL);
	feed(0x8001, 0, 0x71, 0x72);
	feed(0x8002, 0, 0x73, 0x74);
	m17VoicePoll();
	CHECK(!block() && spk_on, "a final-frame flag suppressed the lead");
	CHECK(block() && played[nplayed - 1] == 0x71, "bit 15 alone was treated as a signature");
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	decodes_before = decodes;
	feed(0x1234, -1, 0, 0);
	m17VoicePoll();
	CHECK(starts == 0 && decodes == decodes_before, "a failed decode was queued");
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	set_type((uint16_t)((M17_TYPE_STREAM | M17_TYPE_VOICE) | (1u << M17_TYPE_ENC_SHIFT)));
	feed(1, 0, 0x81, 0x82);
	feed(2, 0, 0x83, 0x84);
	m17VoicePoll();
	CHECK(starts == 0, "an encrypted TYPE was queued");
	set_type(M17_TYPE_STREAM | M17_TYPE_DATA);
	feed(1, 0, 0x81, 0x82);
	feed(2, 0, 0x83, 0x84);
	m17VoicePoll();
	CHECK(starts == 0, "a data-only TYPE was queued");
	set_type(M17_TYPE_STREAM | M17_TYPE_VOICE_DATA);
	feed(1, 0, 0x81, 0x82);
	feed(2, 0, 0x83, 0x84);
	m17VoicePoll();
	CHECK(starts == 0, "a voice-and-data TYPE was queued");
	set_type((uint16_t)((M17_TYPE_STREAM | M17_TYPE_VOICE) | M17_TYPE_SIGNED_MASK));
	play_one(0x91, 0x92, 0x93, 0x94);
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	hk->rx_start(&rx, NULL);
	feed(1, 0, 0xa1, 0xa2);
	hk->rx_end(NULL);
	decodes_before = decodes;
	CHECK(!block() && decodes == decodes_before && !spk_on, "a call shorter than four frames played");
	m17VoiceEnd();

	begin_at(NULL, NULL, NULL);
	feed(1, 0, 0xb1, 0xb2);
	feed(2, 0, 0xb3, 0xb4);
	m17VoicePoll();
	CHECK(starts == 0 && !block(), "voice memory of 0 started the stream");
}

static void test_rollover(void *scratch, void *state, void *queues)
{
	uint8_t n = 1;

	begin_at(scratch, state, queues);
	hk->rx_start(&rx, NULL);
	for (int i = 0; i < 8; i++, n = (uint8_t)(n + 2))
		feed((uint16_t)(i + 1), 0, n, (uint8_t)(n + 1));
	m17VoicePoll();
	CHECK(!block(), "lead");
	for (int i = 0; i < 8; i++)
		CHECK(block() && played[nplayed - 1] == (uint8_t)(i + 1), "early %d", i);
	for (int i = 0; i < 4; i++, n = (uint8_t)(n + 2))
		feed((uint16_t)(i + 9), 0, n, (uint8_t)(n + 1));
	for (int i = 9; i <= 24; i++)
		CHECK(block() && played[nplayed - 1] == (uint8_t)i, "wrapped %d played %u", i,
			(unsigned)played[nplayed - 1]);
	m17VoiceEnd();
}

static void test_retune(void *scratch, void *state, void *queues)
{
	int base;

	begin_at(scratch, state, queues);
	base = play_one(0x31, 0x32, 0x33, 0x34);
	m17VoiceHalt();
	CHECK(!block() && !spk_on, "a retune played the old channel");
	(void)base;
	play_one(0x41, 0x42, 0x43, 0x44);
	CHECK(played[nplayed - 1] == 0x41, "the new channel did not start clean");
	m17VoiceEnd();
}

static void test_transitions(void *scratch, void *state, void *queues)
{
	begin_at(scratch, state, queues);
	play_one(0x31, 0x32, 0x33, 0x34);
	hk->rx_end(NULL);
	set_type(M17_TYPE_STREAM | M17_TYPE_DATA);
	hk->rx_start(&rx, NULL);
	CHECK(!block() && !spk_on, "a non-voice call left the previous voice speaker route enabled");
	m17VoiceEnd();

	begin_at(scratch, state, queues);
	play_one(0x31, 0x32, 0x33, 0x34);
	m17VoiceHalt();
	hk->rx_start(&rx, NULL);
	feed(1, 0, 0x41, 0x42);
	feed(2, 0, 0x43, 0x44);
	CHECK(!block(), "the retuned call did not prime silently");
	CHECK(block() && played[nplayed - 1] == 0x41, "retune discarded the new call's already queued frames");
	m17VoiceEnd();
}

static void test_bench(void *scratch, void *state, void *queues)
{
	int i;

	m17VoiceEnd();
	CHECK(m17VoiceBenchEnter(), "bench could not take a free decoder");
	i = inits;
	m17VoiceBegin(scratch, state, queues, &rx);
	CHECK(inits == i, "begin initialized the decoder the bench was using");
	set_type(M17_TYPE_STREAM | M17_TYPE_VOICE);
	hk->rx_start(&rx, NULL);
	feed(1, 0, 0x11, 0x12);
	feed(2, 0, 0x13, 0x14);
	m17VoicePoll();
	CHECK(!block(), "a block ran while the bench held the decoder");
	CHECK(!spk_on, "the speaker came on without the decoder");
	m17VoiceBenchLeave();
	CHECK(!block() && spk_on, "the lead did not run once the bench released");
	CHECK(block() && played[nplayed - 1] == 0x11, "samples after the bench");
	CHECK(!m17VoiceBenchEnter(), "the bench took the decoder from a live call");
	m17VoiceEnd();
	CHECK(m17VoiceBenchEnter(), "end did not release the decoder");
	m17VoiceBenchLeave();
}

int main(void)
{
	static uint64_t scratch[8];
	static uint64_t state[8];
	static uint64_t queues[32];
	static uint64_t region[40];
	void *lent_q = (uint8_t *)region + 16;

	m17VoiceInit(&audio);
	hk = m17VoiceHooks();
	CHECK(hk->rx_voice_fec != NULL && hk->rx_packet == NULL, "hooks");

	test_playback(scratch, state, queues, "static");
	test_playback(region, region, lent_q, "lend");
	test_rules(scratch, state, queues);
	test_rollover(scratch, state, queues);
	test_retune(scratch, state, queues);
	test_bench(scratch, state, queues);
	test_transitions(scratch, state, queues);

	printf("# m17VoiceTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
