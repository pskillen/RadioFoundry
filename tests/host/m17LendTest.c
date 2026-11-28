// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#define main m17_radio_tests
#include "m17RadioTest.c"
#undef main

#include "c2fx.h"
#include "debuglink/debuglink_m17_bench.h"
#include "nxdn/nxdn_engine.h"
#include "radio/radio_nxdn.h"

static uint64_t nxdn_mem[(sizeof(nxdn_engine_t) + 7u) / 8u];
static bool nxdn_lends;

void *radio_nxdn_idle_mem(size_t *len)
{
	*len = nxdn_lends ? sizeof(nxdn_engine_t) : 0;
	return nxdn_lends ? nxdn_mem : NULL;
}

#if CONFIG_DEBUGLINK
static void (*audio_job)(void);
static bool armed;
static void *last_dec, *last_scratch;

static bool defer_audio(void (*job)(void))
{
	if (audio_job != NULL)
		return false;
	audio_job = job;
	return true;
}

bool debuglink_m17_bench_armed(void)
{
	return armed;
}

bool debuglink_m17_bench_audio(void *dec, void *scratch, int16_t pcm[AUDIO_BLOCK_SAMPLES])
{
	last_dec = dec;
	last_scratch = scratch;
	armed = false;
	return dec != NULL && scratch != NULL;
}

static void run_audio_job(void)
{
	if (audio_job != NULL) {
		void (*job)(void) = audio_job;
		audio_job = NULL;
		job();
	}
}
#endif

static void check_session(const void *engine, bool has_voice)
{
	radio_m17_settings_t settings;
	radio_m17_view_t view = { 0 };

	radio_m17_settings(&settings);
	CHECK(radio_m17_begin(&settings, 0));
#if CONFIG_M17_STATIC_CODEC
	(void)engine;
	CHECK(radio_m17_engine_mem() == voc_mem);
#else
	CHECK(radio_m17_engine_mem() == engine);
#endif
	radio_m17_poll(&view);
	CHECK((view.flags & RADIO_M17_ON) != 0);
#if CONFIG_M17_STATIC_CODEC
	has_voice = true;
#endif
	CHECK(((view.flags & RADIO_M17_VOICE_MEM) != 0) == has_voice);
#if CONFIG_DEBUGLINK
	radio_m17_bench_service();
	CHECK(audio_job == NULL);	// no request
	armed = true;
	radio_m17_bench_service();
	CHECK((audio_job != NULL) == has_voice);
	if (has_voice) {
		run_audio_job();
#if CONFIG_M17_STATIC_CODEC
		CHECK(last_scratch != NULL && last_dec != NULL);
		CHECK(last_scratch != voc_mem && last_dec != voc_mem);
		CHECK(((uintptr_t)last_scratch & 7u) == 0);
#else
		CHECK(last_scratch == voc_mem);
		CHECK(last_dec == (uint8_t *)voc_mem + C2FX_SCRATCH_BYTES);
#endif
		armed = true;
		radio_m17_bench_service();
		CHECK(audio_job != NULL);
	}
#endif
	radio_m17_end();
#if CONFIG_DEBUGLINK
	run_audio_job();	// a delayed audio wake must not touch the released lend
	CHECK(last_scratch == NULL && last_dec == NULL);
	armed = false;
#endif
	radio_m17_poll(&view);
	CHECK((view.flags & (RADIO_M17_ON | RADIO_M17_VOICE_MEM)) == 0);
}

static void test_ccm_and_p25_pool(void)
{
	radio_lend_t blocks[RADIO_LEND_MAX];
	radio_m17_voice_mem_t voice;
	size_t state_bytes = C2FX_DEC_BYTES + RADIO_M17_VOICE_QUEUE_BYTES;

	voc_on = true;
	voc_p = voc_mem;
	voc_len = 5856;
	p25_return = p25_mem;
	p25_len = 5352;
	nxdn_lends = false;
	uint8_t n = radio_lend_gather(blocks);
	CHECK(n == 2 && blocks[0].p == voc_mem && blocks[1].p == p25_mem);
	void *engine = radio_m17_lend_place(blocks, n, &voice);

#ifdef C2FX_NO_ENCODER
	CHECK(C2FX_SCRATCH_BYTES == 4504);
	CHECK(engine == p25_mem);
	CHECK(voice.scratch == voc_mem);
	CHECK(voice.state == (uint8_t *)voc_mem + C2FX_SCRATCH_BYTES);
	CHECK(C2FX_SCRATCH_BYTES + state_bytes <= voc_len);
	check_session(p25_mem, true);
#else
	CHECK(C2FX_SCRATCH_BYTES == 5392);
	state_bytes += C2FX_ENC_BYTES;
	CHECK(C2FX_SCRATCH_BYTES <= voc_len);
	CHECK(C2FX_SCRATCH_BYTES + state_bytes > voc_len);
	CHECK(radio_m17_engine_bytes() + state_bytes > p25_len);
	CHECK(engine == voc_mem && voice.scratch == NULL && voice.state == NULL);
	check_session(voc_mem, false);
#endif
}

static void test_fixed_vocoder_pool(void)
{
	radio_lend_t blocks[RADIO_LEND_MAX];
	radio_m17_voice_mem_t voice;

	voc_on = true;
	voc_p = voc_mem;
	voc_len = sizeof voc_mem;
	p25_return = NULL;
	p25_len = 0;
	nxdn_lends = true;
	uint8_t n = radio_lend_gather(blocks);
	CHECK(n == 2 && blocks[0].p == voc_mem && blocks[1].p == nxdn_mem);
	void *engine = radio_m17_lend_place(blocks, n, &voice);

	CHECK(C2FX_SCRATCH_BYTES + radio_m17_engine_bytes() > voc_len);
	CHECK(radio_m17_engine_bytes() > sizeof(nxdn_engine_t));
	CHECK(engine == voc_mem && voice.scratch == NULL && voice.state == NULL);
	check_session(voc_mem, false);
	CHECK(radio_m17_lend_place(blocks + 1, 1, &voice) == NULL);
	CHECK(voice.scratch == NULL && voice.state == NULL);
}

int main(void)
{
	radio_m17_init();
	test_ccm_and_p25_pool();
	test_fixed_vocoder_pool();
	printf("# m17LendTest scratch=%zu engine=%zu nxdn=%zu: %d checks, %s\n",
		C2FX_SCRATCH_BYTES, radio_m17_engine_bytes(), sizeof(nxdn_engine_t),
		checks, failures ? "FAILED" : "PASSED");
	return failures ? 1 : 0;
}
