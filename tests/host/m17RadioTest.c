// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The M17 radio module's lend (core/radio/radio_m17.c, radio_lend.c): the engine block fits a P25 block and a
// vocoder block on their own, and a block that cannot hold it does not start the session. This file does not
// link radio_sm.c. The vocoder stand-in in radioCoreTest.c is compiled into that binary.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
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
	(void)cb;
	(void)ctx;
	return true;
}
static void a_close(void) {}
static void a_restart(void) {}

static const hal_air_t fake_air = {
	.fit = a_fit, .open = a_open, .close = a_close, .rx_restart = a_restart,
};

const board_t board = { .name = "m17-lend", .air = &fake_air, .vocoder = &fake_vocoder };

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
	channel_t ch;
	const void *p;

	memset(&ch, 0, sizeof ch);
	voc_on = false;
	p25_return = p25_mem;
	p25_len = sizeof p25_mem;
	radio_m17_init();
	radio_m17_reset_stats();
	CHECK(radio_m17_begin(&ch, 0));
	p = radio_m17_engine_mem();
	CHECK(in_block(p, p25_mem, sizeof p25_mem));
	CHECK(radio_m17_running());
	radio_m17_end();
}

static void test_lend_vocoder_block(void)
{
	channel_t ch;
	const void *p;

	memset(&ch, 0, sizeof ch);
	voc_on = true;
	voc_p = voc_mem;
	voc_len = sizeof voc_mem;
	p25_return = NULL;
	p25_len = 0;
	radio_m17_init();
	radio_m17_reset_stats();
	CHECK(radio_m17_begin(&ch, 0));
	p = radio_m17_engine_mem();
	CHECK(in_block(p, voc_mem, sizeof voc_mem));
	CHECK(radio_m17_running());
	radio_m17_end();
}

static void test_lend_too_small(void)
{
	channel_t ch;
	radio_m17_stats_t st;

	memset(&ch, 0, sizeof ch);
	voc_on = true;
	voc_p = tiny_mem;
	voc_len = sizeof tiny_mem;
	p25_return = NULL;
	p25_len = 0;
	radio_m17_init();
	radio_m17_reset_stats();
	CHECK(!radio_m17_begin(&ch, 0));
	CHECK(!radio_m17_running());
	CHECK(radio_m17_engine_mem() == NULL);
	radio_m17_stats(&st);
	CHECK(st.begins == 1);
	CHECK(st.begin_fails == 1);
}

int main(void)
{
	test_mode_value();
	test_lend_engine_size();
	test_lend_p25_block();
	test_lend_vocoder_block();
	test_lend_too_small();
	printf("m17RadioTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
