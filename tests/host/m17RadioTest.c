// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 radio module's lend (core/radio/radio_m17.c, radio_lend.c): the engine block fits a P25 block and a
// vocoder block on their own, and a block that cannot hold it does not start the session. This file does not
// link radio_sm.c. The vocoder stand-in in radioCoreTest.c is compiled into that binary.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "air/air_syms.h"
#include "m17/m17.h"
#include "m17/m17_codeplug.h"
#include "radio/radio_m17.h"

static int failures, checks;
#define CHECK(c)                                                                                                               \
	do {                                                                                                                 \
		checks++;                                                                                                      \
		if (!(c)) {                                                                                                    \
			failures++;                                                                                            \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
		}                                                                                                              \
	} while (0)

static uint64_t p25_mem[6640 / 8];
static uint64_t voc_mem[7168 / 8];
static uint64_t tiny_mem[64 / 8];

static bool voc_on;
static void *voc_p;
static size_t voc_len;
static void *p25_return;
static size_t p25_len;
static air_event_cb_t air_event;
static void *air_ctx;
static uint32_t air_time;

static void *v_lend(size_t *len)
{
	if (!voc_on) {
		*len = 0;
		return NULL;
	}
	*len = voc_len;
	return voc_p;
}

static bool v_present(void) { return false; }
static void v_reset(void) {}
static void v_encode(const int16_t *pcm, uint8_t *bits) { (void)pcm; (void)bits; }
static void v_decode(const uint8_t *bits, int16_t *pcm) { (void)bits; (void)pcm; }

static const hal_vocoder_t fake_vocoder = {
	.present = v_present, .reset = v_reset, .encode = v_encode, .decode = v_decode, .lend = v_lend,
};

static uint32_t a_fit(const air_profile_t *p)
{
	(void)p;
	return AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;
}
static bool a_open(const air_profile_t *p, air_event_cb_t cb, void *ctx)
{
	(void)p;
	air_event = cb;
	air_ctx = ctx;
	air_time = 1000000;
	return true;
}
static void a_close(void) {}
static void a_restart(void) {}

static const hal_air_t fake_air = {
	.fit = a_fit, .open = a_open, .close = a_close, .rx_restart = a_restart,
};

#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
static bool defer_audio(void (*job)(void));
static const hal_audio_t fake_audio = { .defer = defer_audio };
#endif

const board_t board = {
	.name = "m17-lend", .air = &fake_air, .vocoder = &fake_vocoder,
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	.audio = &fake_audio,
#endif
};

bool m17_chan_load(uint16_t number, const channel_t *ch, m17_chan_t *out)
{
	(void)number;
	(void)ch;
	(void)out;
	return false;
}

bool m17_callsign_load(const char *radio_name, char out[M17_CP_CALL_LEN])
{
	(void)radio_name;
	out[0] = '\0';
	return false;
}

// P25's own block, stood in for here: the firmware's returns NULL while P25 itself is running
void *radio_p25_idle_mem(size_t *len)
{
	*len = p25_len;
	return p25_return;
}

static bool in_block(const void *p, const void *base, size_t len)
{
	uintptr_t a = (uintptr_t)p;
	uintptr_t b = (uintptr_t)base;

	return p != NULL && (a & 7u) == 0 && a >= b && a + radio_m17_engine_bytes() <= b + len;
}

static void test_mode_value(void)
{
	CHECK(RADIO_MODE_M17 == 4);
	CHECK(RADIO_MODE_NXDN == 3);
}

static void test_lend_engine_size(void)
{
	size_t n = radio_m17_engine_bytes();

	printf("m17 engine block %zu bytes\n", n);
	CHECK(n <= 4096);
}

static void test_lend_p25_block(void)
{
	radio_m17_settings_t base;
	const void *p;

	voc_on = false;
	p25_return = p25_mem;
	p25_len = sizeof p25_mem;
	radio_m17_init();
	radio_m17_reset_stats();
	radio_m17_settings(&base);
	CHECK(radio_m17_begin(&base, 0));
	p = radio_m17_engine_mem();
	CHECK(in_block(p, p25_mem, sizeof p25_mem));
	CHECK(radio_m17_running());
	radio_m17_end();
}

static void test_lend_vocoder_block(void)
{
	radio_m17_settings_t base;
	const void *p;

	voc_on = true;
	voc_p = voc_mem;
	voc_len = sizeof voc_mem;
	p25_return = NULL;
	p25_len = 0;
	radio_m17_init();
	radio_m17_reset_stats();
	radio_m17_settings(&base);
	CHECK(radio_m17_begin(&base, 0));
	p = radio_m17_engine_mem();
	CHECK(in_block(p, voc_mem, sizeof voc_mem));
	CHECK(radio_m17_running());
	radio_m17_end();
}

static void test_lend_too_small(void)
{
	radio_m17_settings_t base;
	radio_m17_stats_t st;

	voc_on = true;
	voc_p = tiny_mem;
	voc_len = sizeof tiny_mem;
	p25_return = NULL;
	p25_len = 0;
	radio_m17_init();
	radio_m17_reset_stats();
	radio_m17_settings(&base);
	CHECK(!radio_m17_begin(&base, 0));
	CHECK(!radio_m17_running());
	CHECK(radio_m17_engine_mem() == NULL);
	radio_m17_stats(&st);
	CHECK(st.begins == 1);
	CHECK(st.begin_fails == 1);
}

static void inject(const uint8_t packed[48])
{
	for (unsigned i = 0; i < 48; i += 12) {
		int8_t soft[48];
		for (uint16_t j = 0; j < 48; j++)
			soft[j] = (int8_t)(air_sym_level(air_sym_get(packed + i, j)) * 32);
		const air_chunk_t chunk = { .t_us = air_time, .nsyms = 48, .sync_at = -1, .hard = packed + i, .soft = soft };
		const air_event_t event = { .kind = AIR_EV_RX, .chunk = &chunk };
		air_event(&event, air_ctx);
		air_time += 10000;
	}
}

static void inject_lsf(uint8_t lsf[M17_LSF_BYTES])
{
	uint8_t frame[48];

	memset(frame, 0x77, sizeof frame);
	inject(frame);
	frame[0] = 0x55;
	frame[1] = 0xF7;
	m17_lsf_encode(lsf, frame + 2);
	inject(frame);
}

static void make_lsf(uint8_t lsf[M17_LSF_BYTES], uint16_t type, uint8_t control, const char *text)
{
	uint8_t encoded[M17_PAYLOAD_BYTES];

	memset(lsf, 0, M17_LSF_BYTES);
	m17_addr_encode("ALL", lsf);
	m17_addr_encode("AB1CD", lsf + M17_ADDR_BYTES);
	lsf[M17_LSF_TYPE_OFFSET] = (uint8_t)(type >> 8);
	lsf[M17_LSF_TYPE_OFFSET + 1] = (uint8_t)type;
	lsf[M17_LSF_META_OFFSET] = control;
	if (text != NULL) {
		memset(lsf + M17_LSF_META_OFFSET + 1, ' ', M17_META_TEXT_BLOCK_BYTES);
		memcpy(lsf + M17_LSF_META_OFFSET + 1, text, strlen(text));
	}
	m17_lsf_encode(lsf, encoded);
}

static void inject_stream(const uint8_t lsf[M17_LSF_BYTES], uint8_t start, uint8_t stop, uint16_t *fn)
{
	uint8_t frame[48] = { 0xFF, 0x5D };
	const uint8_t voice[16] = { 0 };

	for (uint8_t i = start; i < stop; i++) {
		m17_stream_encode(lsf, i, (*fn)++, voice, frame + 2);
		inject(frame);
	}
}

static void test_receive_snapshot(void)
{
	radio_m17_settings_t base = { .can = 3, .can_squelch = true };
	radio_m17_view_t v = { 0 };
	uint8_t a[M17_LSF_BYTES], b[M17_LSF_BYTES], encoded[M17_PAYLOAD_BYTES];
	uint16_t fn = 0, type = (7u << M17_TYPE_CAN_SHIFT) | M17_TYPE_STREAM | M17_TYPE_VOICE;

	voc_on = false;
	p25_return = p25_mem;
	p25_len = sizeof p25_mem;
	radio_m17_init();
	CHECK(radio_m17_begin(&base, 0));
	make_lsf(a, type, 0x32, "ond block");
	inject_stream(a, 0, 2, &fn);
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_UNKNOWN && v.rx_can == UINT8_MAX);
	CHECK(v.caller[0] == 0 && v.dest[0] == 0 && v.meta[0] == 0);
	inject_stream(a, 2, 3, &fn);
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_OTHER_CAN && v.rx_can == 7);
	CHECK(v.can == 3 && v.caller[0] == 0 && v.meta[0] == 0);
	inject_stream(a, 3, 6, &fn);
	CHECK(radio_m17_poll(&v) && (v.flags & RADIO_M17_LATE));
	CHECK(strcmp(v.caller, "AB1CD") == 0 && strcmp(v.dest, "ALL") == 0 && strcmp(v.meta, "ond block") == 0);
	base.can_squelch = false;
	CHECK(radio_m17_apply(&base, 0));
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_VOICE);

	make_lsf(b, type, 0x31, "First and sec");
	inject_stream(b, 0, 3, &fn);
	inject_stream(b, 0, 1, &fn);
	inject_stream(b, 3, 6, &fn);
	CHECK(!radio_m17_poll(&v));
	b[M17_LSF_BYTES - 1] ^= 1;
	inject_stream(b, 0, 6, &fn);
	CHECK(!radio_m17_poll(&v));
	m17_lsf_encode(b, encoded);
	inject_stream(b, 0, 6, &fn);
	CHECK(radio_m17_poll(&v) && strcmp(v.meta, "First and second block") == 0);
	inject_stream(b, 0, 6, &fn);
	CHECK(!radio_m17_poll(&v));
	make_lsf(b, type, 0x34, "Out of mask");
	inject_stream(b, 0, 6, &fn);
	CHECK(!radio_m17_poll(&v));
	make_lsf(b, type, 0x32, "New caller");
	m17_addr_encode("AB2CD", b + M17_ADDR_BYTES);
	m17_lsf_encode(b, encoded);
	inject_stream(b, 0, 6, &fn);
	CHECK(radio_m17_poll(&v) && strcmp(v.caller, "AB2CD") == 0 && strcmp(v.meta, "New caller") == 0);
	make_lsf(b, type, 0, NULL);
	m17_addr_encode("AB2CD", b + M17_ADDR_BYTES);
	m17_lsf_encode(b, encoded);
	inject_stream(b, 0, 6, &fn);
	CHECK(radio_m17_poll(&v) && v.meta[0] == 0);

	radio_m17_retuned();
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_NONE && v.caller[0] == 0);
	make_lsf(a, type | (1u << M17_TYPE_ENC_SHIFT), 0x11, "Hidden");
	inject_lsf(a);
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_ENCRYPTED && v.meta[0] == 0);
	CHECK(strcmp(v.caller, "AB1CD") == 0 && !(v.flags & RADIO_M17_LATE));

	radio_m17_retuned();
	make_lsf(a, type | M17_TYPE_META_CALL, 0, NULL);
	m17_addr_encode("AB2CD", a + M17_LSF_META_OFFSET);
	inject_lsf(a);
	CHECK(radio_m17_poll(&v) && strcmp(v.caller, "AB2CD") == 0 && strcmp(v.via, "AB1CD") == 0);
	make_lsf(b, type | M17_TYPE_META_CALL, 0, NULL);
	inject_stream(b, 0, 6, &fn);
	CHECK(radio_m17_poll(&v) && strcmp(v.caller, "AB1CD") == 0 && v.via[0] == 0);

	radio_m17_retuned();
	make_lsf(a, 3u << M17_TYPE_CAN_SHIFT, 0x31, "Not a packet");
	inject_lsf(a);
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_DATA && v.meta[0] == 0);
	radio_m17_retuned();
	make_lsf(a, 3u << M17_TYPE_CAN_SHIFT, 0x11, "Caf\xC3\xA9");
	inject_lsf(a);
	CHECK(radio_m17_poll(&v) && strcmp(v.meta, "Caf\xE9") == 0);
	uint8_t eot[48];
	for (unsigned i = 0; i < sizeof eot; i += 2) {
		eot[i] = 0x55;
		eot[i + 1] = 0x5D;
	}
	inject(eot);
	CHECK(radio_m17_poll(&v) && v.rx_state == RADIO_M17_RX_NONE && v.meta[0] == 0);
	radio_m17_end();
}

int main(void)
{
	test_mode_value();
	test_lend_engine_size();
	test_lend_p25_block();
	test_lend_vocoder_block();
	test_lend_too_small();
	test_receive_snapshot();
	printf("m17RadioTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
