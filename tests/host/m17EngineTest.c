// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 receive on the framer (docs/spec/m17.md 5.2, 5.5): a sync is kept only when the head hook can prove the
// frame, and a call's LSF is rebuilt from LICH chunks by their counter.
#include <stdio.h>
#include <string.h>

#include "air/air_framer.h"
#include "air/air_syms.h"
#include "m17/m17_rx.h"

static int failures, checks;
#define CHECK(c, ...)                                                                                                          \
	do {                                                                                                                 \
		checks++;                                                                                                      \
		if (!(c)) {                                                                                                    \
			failures++;                                                                                            \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
			printf(__VA_ARGS__);                                                                                   \
			printf("\n");                                                                                          \
		}                                                                                                              \
	} while (0)

#define SYM_MAX 8000
static uint8_t sym[SYM_MAX];
static uint32_t sn;
static uint32_t t_us;
static int ends;

static uint8_t fbuf[M17_FRAME_SYMS / 4];
static int8_t fsoft[M17_FRAME_SYMS];
static air_framer_t fr;
static m17_rx_t rx;

static void on_end(void *ctx)
{
	(void)ctx;
	ends++;
}

static const air_sync_t *sync_of(uint8_t id)
{
	for (uint8_t i = 0; i < m17_air_mode.profile.nsyncs; i++)
		if (m17_air_mode.profile.sync[i].id == id)
			return &m17_air_mode.profile.sync[i];
	return NULL;
}

static void emit_dibit_byte(uint8_t b, uint16_t nsyms)
{
	for (uint16_t i = 0; i < nsyms; i++)
		sym[sn++] = air_sym_get(&b, (uint16_t)(i & 3));
}

// A frame as on air: a start-only pattern gets the rest of a 192-symbol preamble ahead of its tail.
static void emit_frame(uint8_t id, const uint8_t pay[M17_PAYLOAD_BYTES])
{
	const air_sync_t *s = sync_of(id);

	if (s->flags & AIR_SYNC_START_ONLY) {
		uint8_t pre = s->syms[0];

		emit_dibit_byte(pre, (uint16_t)(M17_PREAMBLE_SYMS - s->frame_at));
	}
	for (uint8_t i = 0; i < s->nsyms; i++)
		sym[sn++] = air_sym_get(s->syms, i);
	for (uint16_t i = 0; i < M17_PAYLOAD_BITS / 2; i++)
		sym[sn++] = air_sym_get(pay, i);
}

static void emit_eot(void)
{
	const air_sync_t *s = sync_of(M17_SYNC_ID_EOT);

	for (uint16_t n = 0; n < M17_FRAME_SYMS; n++)
		sym[sn++] = air_sym_get(s->syms, (uint8_t)(n % s->nsyms));
}

static void begin(void)
{
	m17_rx_init(&rx, on_end, NULL);
	// Soft values, as the discriminator tap does. A hard slice takes the BERT preamble word for an LSF
	// preamble (they are two symbols apart); the level fit does not.
	air_framer_init(&fr, &m17_air_mode, &rx, fbuf, fsoft, M17_FRAME_SYMS);
	sn = 0;
	t_us = 1000000;
	ends = 0;
}

static void feed(void)
{
	uint32_t at = 0;

	while (at < sn) {
		uint8_t hard[AIR_CHUNK_SYMS_MAX / 4];
		int8_t soft[48];
		uint16_t n = (uint16_t)(sn - at > 48 ? 48 : sn - at);
		air_chunk_t c;

		memset(hard, 0, sizeof hard);
		memset(&c, 0, sizeof c);
		c.t_us = t_us;
		c.nsyms = n;
		c.sync_at = -1;
		c.hard = hard;
		c.soft = soft;
		for (uint16_t i = 0; i < n; i++) {
			air_sym_put(hard, i, sym[at + i]);
			soft[i] = (int8_t)(air_sym_level(sym[at + i]) * 32);
		}
		air_framer_rx(&fr, &c);
		t_us += n * 208u;
		at += n;
	}
	sn = 0;
}

static void make_lsf(uint8_t lsf[M17_LSF_BYTES], const char *src, uint8_t type0)
{
	uint8_t addr[M17_ADDR_BYTES];
	uint8_t dst[M17_ADDR_BYTES];

	memset(lsf, 0x11, M17_LSF_BYTES);
	memset(dst, 0xFF, sizeof dst);
	memcpy(lsf, dst, M17_ADDR_BYTES);
	CHECK(m17_addr_encode(src, addr), "addr %s", src);
	memcpy(lsf + M17_ADDR_BYTES, addr, M17_ADDR_BYTES);
	lsf[12] = type0;
	lsf[13] = 0x00;
}

// Plain bit p sits at air bit qpp(p) after the interleaver; the randomizer does not move it.
static void flip_plain(uint8_t pay[M17_PAYLOAD_BYTES], uint16_t plain)
{
	uint16_t air = m17_interleave_qpp(plain);

	pay[air >> 3] ^= (uint8_t)(0x80u >> (air & 7));
}

static void test_lsf_crc_accepts(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES];
	uint16_t rejects;

	begin();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	emit_frame(M17_SYNC_ID_LSF, pay);
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 1 && fr.stats.rejects == rejects, "good LSF frames %u rejects %u", fr.stats.frames,
	      fr.stats.rejects);
	CHECK(rx.in_call && rx.crc_ok && !memcmp(rx.lsf + 6, lsf + 6, 6), "good LSF kept");
	CHECK(ends == 0, "good LSF ends %d", ends);
}

static void test_lsf_crc_rejects(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES], got[M17_LSF_BYTES];
	m17_fec_work_t w;
	uint16_t rejects;

	begin();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	for (uint16_t i = 0; i < M17_PAYLOAD_BITS; i += 7)
		flip_plain(pay, i);
	CHECK(m17_lsf_decode(pay, &w, got) < 0, "payload CRC really fails");
	emit_frame(M17_SYNC_ID_LSF, pay);
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 0 && fr.stats.rejects > rejects, "bad LSF frames %u rejects %u (was %u)", fr.stats.frames,
	      fr.stats.rejects, rejects);
	CHECK(!rx.in_call, "bad LSF started a call");
}

static void test_stream_lich_ok(void)
{
	uint8_t lsf[M17_LSF_BYTES], pl[16], pay[M17_PAYLOAD_BYTES];
	uint16_t rejects;

	begin();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	memset(pl, 0x5A, sizeof pl);
	m17_stream_encode(lsf, 3, 9, pl, pay);
	emit_frame(M17_SYNC_ID_STREAM, pay);
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 1 && fr.stats.rejects == rejects, "LICH frames %u rejects %u", fr.stats.frames,
	      fr.stats.rejects);
	CHECK(rx.have[3] && !memcmp(rx.lsf + 15, lsf + 15, 5), "chunk 3");
	CHECK(rx.in_call, "stream locks");
}

static void test_stream_lich_three_bits(void)
{
	uint8_t lsf[M17_LSF_BYTES], pl[16], pay[M17_PAYLOAD_BYTES], clean[M17_PAYLOAD_BYTES];
	uint16_t rejects;

	begin();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	memset(pl, 0x5A, sizeof pl);
	m17_stream_encode(lsf, 3, 1, pl, pay);
	memcpy(clean, pay, sizeof clean);
	flip_plain(pay, 0);
	flip_plain(pay, 1);
	emit_frame(M17_SYNC_ID_STREAM, pay);
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 1 && fr.stats.rejects == rejects, "2-bit LICH frames %u rejects %u", fr.stats.frames,
	      fr.stats.rejects);

	begin();
	memcpy(pay, clean, sizeof pay);
	flip_plain(pay, 0);
	flip_plain(pay, 1);
	flip_plain(pay, 2);
	emit_frame(M17_SYNC_ID_STREAM, pay);
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 0 && fr.stats.rejects > rejects, "3-bit LICH frames %u rejects %u (was %u)",
	      fr.stats.frames, fr.stats.rejects, rejects);
}

static void emit_stream(const uint8_t lsf[M17_LSF_BYTES], uint8_t counter, uint16_t fn)
{
	uint8_t pl[16], pay[M17_PAYLOAD_BYTES];

	memset(pl, 0x5A, sizeof pl);
	m17_stream_encode(lsf, counter, fn, pl, pay);
	emit_frame(M17_SYNC_ID_STREAM, pay);
}

static void test_late_entry(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES];

	begin();
	make_lsf(lsf, "AB1CD", 0x42);
	m17_lsf_encode(lsf, pay);
	// Frame numbers 10..15 are not the counters modulo 6 (10 % 6 == 4).
	for (uint8_t n = 0; n < 3; n++)
		emit_stream(lsf, n, (uint16_t)(10 + n));
	feed();
	CHECK(rx.late, "late after chunk 2");
	CHECK(rx.have[2] && !memcmp(rx.lsf + 10, lsf + 10, 5), "bytes 10-14");
	CHECK(!rx.crc_ok && !rx.have[5], "not whole yet");
	for (uint8_t n = 3; n < 6; n++)
		emit_stream(lsf, n, (uint16_t)(10 + n));
	feed();
	CHECK(rx.crc_ok, "CRC after chunk 5");
	CHECK(!memcmp(rx.lsf + 6, lsf + 6, 6), "source");

	// A hole: frame number 2 carries counter 3, so the missing chunk must stay missing.
	begin();
	make_lsf(lsf, "AB1CD", 0x42);
	m17_lsf_encode(lsf, pay);
	emit_stream(lsf, 0, 0);
	emit_stream(lsf, 1, 1);
	emit_stream(lsf, 3, 2);
	emit_stream(lsf, 4, 3);
	emit_stream(lsf, 5, 4);
	feed();
	CHECK(!rx.have[2] && !rx.late, "hole: chunk 2 not invented");
	CHECK(memcmp(rx.lsf + 10, lsf + 10, 5) != 0, "hole: bytes 10-14 stay empty");
	CHECK(!rx.crc_ok, "hole: no CRC");
}

static void test_new_lsf_ends_call(void)
{
	uint8_t a[M17_LSF_BYTES], b[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES];

	// The second preamble drops the framer's lock before its LSF sync, so the hook below is what ends a call
	// when a new LSF arrives while the previous one is still held.
	begin();
	make_lsf(a, "AB1CD", 0);
	make_lsf(b, "AB2CD", 0);
	m17_rx_on_lsf(&rx, a);
	m17_rx_on_lsf(&rx, b);
	CHECK(ends == 1, "second LSF ends the call");
	CHECK(!memcmp(rx.lsf + 6, b + 6, 6), "direct second source");

	begin();
	make_lsf(a, "AB1CD", 0);
	make_lsf(b, "AB2CD", 0);
	m17_lsf_encode(a, pay);
	emit_frame(M17_SYNC_ID_LSF, pay);
	feed();
	CHECK(ends == 0 && rx.in_call, "first LSF ends %d", ends);
	m17_lsf_encode(b, pay);
	emit_frame(M17_SYNC_ID_LSF, pay);
	feed();
	CHECK(ends == 1, "second LSF ends %d", ends);
	CHECK(rx.in_call && !memcmp(rx.lsf + 6, b + 6, 6), "second call is AB2CD");
}

static void test_eot_while_locked(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES];
	uint16_t rejects;

	begin();
	emit_eot();
	rejects = fr.stats.rejects;
	feed();
	CHECK(fr.stats.frames == 0 && fr.stats.rejects > rejects, "EOT before a call: frames %u rejects %u",
	      fr.stats.frames, rejects);
	CHECK(ends == 0 && !rx.in_call, "EOT before a call ended it");

	begin();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	emit_frame(M17_SYNC_ID_LSF, pay);
	feed();
	CHECK(rx.in_call && ends == 0, "LSF locked");
	emit_eot();
	rejects = fr.stats.rejects;
	feed();
	CHECK(ends == 1 && !rx.in_call, "two end words end the call (%d)", ends);
	CHECK(fr.stats.rejects == rejects, "end marker rejected %u", fr.stats.rejects);
}

int main(void)
{
	test_lsf_crc_accepts();
	test_lsf_crc_rejects();
	test_stream_lich_ok();
	test_stream_lich_three_bits();
	test_late_entry();
	test_new_lsf_ends_call();
	test_eot_while_locked();
	printf("m17EngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
