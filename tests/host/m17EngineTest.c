// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on the framer and the bytes of a transmission (docs/spec/m17.md 4, 5.2, 5.5, 8): a sync is kept only when
// the head hook can prove the frame, a call's LSF is rebuilt from LICH chunks by their counter, and a
// transmission is a preamble, an LSF, frames and the end marker.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_framer.h"
#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "m17/m17_engine.h"
#include "m17/m17_packet.h"
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

static m17_engine_hooks_t end_hooks;

static int hk_start, hk_voice, hk_lost, hk_end, hk_npkt;
static uint8_t hk_pl[4][16];
static uint16_t hk_fn[4];
static uint8_t hk_pkt[64];
static uint16_t hk_pkt_len;
static m17_engine_hooks_t hk;

static void hk_on_start(const struct m17_rx *r, void *ctx)
{
	(void)r;
	(void)ctx;
	hk_start++;
}

static void hk_on_voice(const uint8_t voice[16], uint16_t frame_num, void *ctx)
{
	(void)ctx;
	if (hk_voice < 4) {
		memcpy(hk_pl[hk_voice], voice, 16);
		hk_fn[hk_voice] = frame_num;
	}
	hk_voice++;
}

static void hk_on_lost(void *ctx)
{
	(void)ctx;
	hk_lost++;
}

static void hk_on_end(void *ctx)
{
	(void)ctx;
	hk_end++;
}

static void hk_on_packet(const uint8_t *data, uint16_t len, void *ctx)
{
	(void)ctx;
	hk_npkt++;
	hk_pkt_len = len;
	if (len && len <= sizeof hk_pkt)
		memcpy(hk_pkt, data, len);
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
	memset(&end_hooks, 0, sizeof end_hooks);
	end_hooks.rx_end = on_end;
	m17_rx_init(&rx, &end_hooks, NULL);
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

static void emit_stream_pl(const uint8_t lsf[M17_LSF_BYTES], uint8_t counter, uint16_t fn, const uint8_t pl[16])
{
	uint8_t pay[M17_PAYLOAD_BYTES];

	m17_stream_encode(lsf, counter, fn, pl, pay);
	emit_frame(M17_SYNC_ID_STREAM, pay);
}

static void emit_stream(const uint8_t lsf[M17_LSF_BYTES], uint8_t counter, uint16_t fn)
{
	uint8_t pl[16];

	memset(pl, 0x5A, sizeof pl);
	emit_stream_pl(lsf, counter, fn, pl);
}

static void begin_hooks(void)
{
	hk_start = hk_voice = hk_lost = hk_end = hk_npkt = 0;
	hk_pkt_len = 0;
	memset(&hk, 0, sizeof hk);
	hk.rx_start = hk_on_start;
	hk.rx_voice = hk_on_voice;
	hk.rx_lost = hk_on_lost;
	hk.rx_end = hk_on_end;
	hk.rx_packet = hk_on_packet;
	m17_rx_init(&rx, &hk, NULL);
	air_framer_init(&fr, &m17_air_mode, &rx, fbuf, fsoft, M17_FRAME_SYMS);
	sn = 0;
	t_us = 1000000;
	ends = 0;
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

static uint16_t pull(m17_engine_t *e, uint8_t *dst, uint16_t n)
{
	uint16_t got = 0;

	while (got < n) {
		uint16_t k = m17_engine_tx_read(e, dst + got, (uint16_t)(n - got));

		if (!k)
			break;
		got = (uint16_t)(got + k);
	}
	return got;
}

static void engine_of(m17_engine_t *e, uint8_t kind, bool (*tx_voice)(uint8_t c2[2][8], void *ctx))
{
	m17_engine_cfg_t cfg;
	m17_engine_hooks_t hooks;

	memset(&cfg, 0, sizeof cfg);
	memset(&hooks, 0, sizeof hooks);
	cfg.own = M17_ADDR_AB1CD;
	cfg.dst = M17_ADDR_BROADCAST;
	cfg.tx_kind = kind;
	hooks.tx_voice = tx_voice;
	m17_engine_init(e, &cfg, &hooks, NULL);
}

static bool voice_seq(uint8_t c2[2][8], void *ctx)
{
	uint8_t *n = ctx;

	memset(c2, 0, 16);
	c2[0][0] = *n;
	(*n)++;
	return true;
}

static bool voice_none(uint8_t c2[2][8], void *ctx)
{
	(void)c2;
	(void)ctx;
	return false;
}

static uint8_t prbs_bit(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void test_stream_order(void)
{
	m17_engine_t e;
	m17_fec_work_t w;
	uint8_t buf[8 * 48];
	uint8_t nvoice = 0;
	uint8_t lich, chunk[5], pl[16];
	uint16_t fn, got;
	int errs;
	uint16_t expect_fn[3] = { 0, 1, 0x8002 };

	engine_of(&e, M17_TX_VOICE, voice_seq);
	e.ctx = &nvoice;
	CHECK(m17_engine_tx_start(&e, 0, NULL, 0), "stream tx starts");
	got = pull(&e, buf, 4 * 48);
	CHECK(got == 4 * 48, "preamble LSF and two frames: %u", got);
	m17_engine_tx_stop(&e);
	got = (uint16_t)(got + pull(&e, buf + got, 2 * 48));
	CHECK(got == 6 * 48, "third frame and end marker: %u", got);
	CHECK(pull(&e, buf, 8) == 0, "nothing after the end marker");
	CHECK(buf[0] == 0x77, "preamble starts %02X", buf[0]);
	for (int i = 0; i < 48; i++)
		if (buf[i] != 0x77) {
			CHECK(0, "preamble byte %d is %02X", i, buf[i]);
			break;
		}
	CHECK(buf[48] == 0x55 && buf[49] == 0xF7, "LSF sync %02X %02X", buf[48], buf[49]);
	CHECK(m17_lsf_decode(buf + 50, &w, e.lsf) >= 0, "LSF decodes");
	for (int f = 0; f < 3; f++) {
		const uint8_t *frame = buf + (2 + f) * 48;

		CHECK(frame[0] == 0xFF && frame[1] == 0x5D, "stream sync %d is %02X %02X", f, frame[0], frame[1]);
		errs = m17_stream_decode(frame + 2, &w, &lich, chunk, &fn, pl, &errs);
		CHECK(errs >= 0 && fn == expect_fn[f], "frame %d number %04X errs %d", f, fn, errs);
	}
	for (int i = 0; i < 48; i += 2)
		if (buf[5 * 48 + i] != 0x55 || buf[5 * 48 + i + 1] != 0x5D) {
			CHECK(0, "end marker at %d", i);
			break;
		}
}

static void test_bert_order(void)
{
	m17_engine_t e;
	m17_prbs9_t g;
	uint8_t buf[4 * 48];
	uint8_t bits[25], pay[M17_PAYLOAD_BYTES];
	uint16_t got;

	engine_of(&e, M17_TX_BERT, NULL);
	CHECK(m17_engine_tx_start(&e, 0, NULL, 0), "bert tx starts");
	got = pull(&e, buf, 2 * 48);
	m17_engine_tx_stop(&e);
	got = (uint16_t)(got + pull(&e, buf + got, 2 * 48));
	CHECK(got == 4 * 48, "preamble, two frames, end: %u", got);
	CHECK(buf[0] == 0xDD, "BERT preamble starts %02X", buf[0]);
	for (int i = 0; i < 48; i++)
		if (buf[i] != 0xDD) {
			CHECK(0, "BERT preamble byte %d is %02X", i, buf[i]);
			break;
		}
	CHECK(!(buf[48] == 0x55 && buf[49] == 0xF7), "BERT has no LSF");
	m17_prbs9_init(&g);
	for (int f = 0; f < 2; f++) {
		const uint8_t *frame = buf + (1 + f) * 48;

		m17_prbs9_bits(&g, bits, M17_BERT_BITS);
		m17_bert_encode_bits(bits, pay);
		CHECK(frame[0] == 0xDF && frame[1] == 0x55, "BERT sync %d is %02X %02X", f, frame[0], frame[1]);
		CHECK(memcmp(frame + 2, pay, M17_PAYLOAD_BYTES) == 0, "BERT frame %d matches the generator", f);
	}
	for (int i = 0; i < 48; i += 2)
		if (buf[3 * 48 + i] != 0x55 || buf[3 * 48 + i + 1] != 0x5D) {
			CHECK(0, "BERT end marker at %d", i);
			break;
		}
}

static void test_silence_fill(void)
{
	m17_engine_t e;
	m17_fec_work_t w;
	uint8_t buf[3 * 48];
	uint8_t lich, chunk[5], pl[16], zeros[16];
	uint16_t fn;
	int lich_errs, errs;

	engine_of(&e, M17_TX_VOICE, voice_none);
	CHECK(m17_engine_tx_start(&e, 0, NULL, 0), "silence tx starts");
	CHECK(pull(&e, buf, 3 * 48) == 3 * 48, "preamble, LSF, one frame");
	errs = m17_stream_decode(buf + 2 * 48 + 2, &w, &lich, chunk, &fn, pl, &lich_errs);
	memset(zeros, 0, sizeof zeros);
	CHECK(errs >= 0 && fn == 0, "silence frame number %04X", fn);
	CHECK(memcmp(pl, zeros, 16) == 0, "payload is 16 zero bytes");
}

static void test_packet_crc(void)
{
	m17_engine_t e;
	m17_fec_work_t w;
	m17_packet_gather_t g;
	uint8_t data[40], buf[6 * 48], frame[25], flipped[25];
	uint8_t fn, valid;
	bool last;
	int nframes = 0;

	for (int i = 0; i < 40; i++)
		data[i] = (uint8_t)(i + 1);
	engine_of(&e, M17_TX_PACKET, NULL);
	CHECK(m17_engine_tx_start(&e, 0x05, data, 40), "40 bytes start");
	CHECK(pull(&e, buf, 5 * 48) == 5 * 48, "preamble, LSF, two frames, end");
	CHECK(buf[0] == 0x77, "packet preamble %02X", buf[0]);
	m17_packet_gather_init(&g);
	for (int f = 0; f < 2; f++) {
		const uint8_t *chunkb = buf + (2 + f) * 48;
		int rc;

		CHECK(chunkb[0] == 0x75 && chunkb[1] == 0xFF, "packet sync %d", f);
		rc = m17_packet_decode(chunkb + 2, &w, frame, &fn, &last, &valid);
		CHECK(rc >= 0, "packet frame %d decodes", f);
		if (f == 0)
			memcpy(flipped, frame, 25);
		rc = m17_packet_gather_frame(&g, frame, last, valid, fn);
		nframes++;
		if (f == 0)
			CHECK(rc == 0 && !last, "first frame waits");
		else
			CHECK(rc == 1 && last, "second frame CRC %d", rc);
	}
	CHECK(nframes == 2, "frames %d", nframes);
	CHECK(g.buf[0] == 0x05, "type %02X", g.buf[0]);
	CHECK(memcmp(g.buf + 1, data, 40) == 0, "gathered data");
	flipped[0] ^= 0x01;
	m17_packet_gather_init(&g);
	m17_packet_gather_frame(&g, flipped, false, 0, 0);
	{
		const uint8_t *chunkb = buf + 3 * 48;
		int rc = m17_packet_decode(chunkb + 2, &w, frame, &fn, &last, &valid);

		CHECK(rc >= 0, "second frame still decodes");
		rc = m17_packet_gather_frame(&g, frame, last, valid, fn);
		CHECK(rc < 0, "flipped bit fails the CRC (%d)", rc);
	}
}

static void test_packet_bounds(void)
{
	m17_engine_t e;
	uint8_t data[900];
	uint8_t buf[40 * 48];
	uint16_t got;
	int frames;

	memset(data, 0x5A, sizeof data);

	engine_of(&e, M17_TX_PACKET, NULL);
	CHECK(m17_engine_tx_start(&e, 0x05, data, 22), "1 frame accepted");
	got = pull(&e, buf, sizeof buf);
	frames = 0;
	for (uint16_t i = 0; i + 48 <= got; i += 48)
		if (buf[i] == 0x75 && buf[i + 1] == 0xFF)
			frames++;
	CHECK(frames == 1, "frames %d (bytes %u)", frames, got);

	engine_of(&e, M17_TX_PACKET, NULL);
	CHECK(m17_engine_tx_start(&e, 0x05, data, 822), "33 frames accepted");
	got = pull(&e, buf, sizeof buf);
	CHECK(got == (33 + 3) * 48, "33-frame transmission length %u", got);
	frames = 0;
	for (uint16_t i = 0; i + 48 <= got; i += 48)
		if (buf[i] == 0x75 && buf[i + 1] == 0xFF)
			frames++;
	CHECK(frames == 33, "frames %d", frames);

	engine_of(&e, M17_TX_PACKET, NULL);
	CHECK(!m17_engine_tx_start(&e, 0x05, data, 823), "34 frames refused");
	CHECK(m17_engine_tx_read(&e, buf, 8) == 0, "refused packet sends nothing");
}

static void test_csma(void)
{
	m17_csma_t c;

	m17_csma_init(&c);
	CHECK(m17_csma_poll(&c, 0, true, false, 1) == M17_CSMA_WAIT, "locked channel sends");
	CHECK(m17_csma_poll(&c, 0, false, false, 1) == M17_CSMA_WAIT, "just-clear channel sends");
	CHECK(m17_csma_poll(&c, 40, false, false, 0) == M17_CSMA_WAIT, "coin 0 sends");
	CHECK(m17_csma_poll(&c, 80, false, false, 0) == M17_CSMA_WAIT, "coin 0 sends");
	CHECK(m17_csma_poll(&c, 120, false, false, 0) == M17_CSMA_WAIT, "coin 0 sends");
	CHECK(m17_csma_poll(&c, 160, false, false, 0) == M17_CSMA_WAIT, "coin 0 sends");
	CHECK(m17_csma_poll(&c, 200, false, false, 32) == M17_CSMA_SEND, "coin under 1/4 waits");

	m17_csma_init(&c);
	CHECK(m17_csma_poll(&c, 0, false, true, 1) == M17_CSMA_WAIT, "signal sends");
	CHECK(m17_csma_poll(&c, 40, false, false, 1) == M17_CSMA_WAIT, "signal just cleared sends");
	CHECK(m17_csma_poll(&c, 80, false, false, 1) == M17_CSMA_SEND, "clear slot waits");

	m17_csma_init(&c);
	CHECK(m17_csma_poll(&c, 0, false, false, 0) == M17_CSMA_WAIT, "wait starts");
	CHECK(m17_csma_poll(&c, 5000, false, false, 0) == M17_CSMA_BUSY, "5 s is not busy");
}

static void test_bert_count(void)
{
	m17_fec_work_t w;
	m17_bert_count_t c;
	m17_prbs9_t g;
	uint8_t bits[25], pay[M17_PAYLOAD_BYTES], dec[25];
	int errs;

	m17_prbs9_init(&g);
	m17_prbs9_bits(&g, bits, M17_BERT_BITS);
	m17_bert_encode_bits(bits, pay);
	memset(dec, 0, sizeof dec);
	errs = m17_bert_decode(pay, &w, dec);
	CHECK(errs >= 0, "BERT decodes");
	m17_bert_count_init(&c);
	m17_bert_count_frame(&c, dec);
	CHECK(c.bits == 197 && c.errors == 0, "clean frame bits %u errors %u", c.bits, c.errors);
	dec[40 >> 3] = (uint8_t)(dec[40 >> 3] ^ (uint8_t)(0x80u >> (40 & 7)));
	m17_bert_count_init(&c);
	m17_bert_count_frame(&c, dec);
	CHECK(c.bits == 197 && c.errors == 1, "flipped bit bits %u errors %u", c.bits, c.errors);
}

static void test_bert_prbs_recurrence(void)
{
	m17_prbs9_t g;
	uint8_t bits[(1022 + 7) / 8];
	int bad = 0;

	m17_prbs9_init(&g);
	m17_prbs9_bits(&g, bits, 1022);
	for (uint16_t n = 9; n < 1022; n++) {
		uint8_t want = (uint8_t)(prbs_bit(bits, (uint16_t)(n - 5)) ^ prbs_bit(bits, (uint16_t)(n - 9)));

		if (prbs_bit(bits, n) != want)
			bad++;
	}
	CHECK(bad == 0, "recurrence mismatches %d", bad);
	bad = 0;
	for (uint16_t i = 0; i < 511; i++)
		if (prbs_bit(bits, i) != prbs_bit(bits, (uint16_t)(i + 511)))
			bad++;
	CHECK(bad == 0, "period 511 mismatches %d", bad);
	for (uint16_t p = 1; p < 511; p++) {
		int same = 1;

		for (uint16_t i = 0; i + p < 1022; i++)
			if (prbs_bit(bits, i) != prbs_bit(bits, (uint16_t)(i + p))) {
				same = 0;
				break;
			}
		if (same) {
			CHECK(0, "period %u also fits", p);
			return;
		}
	}
}

static void test_rx_hooks_call(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES], pl[16];
	uint16_t fns[3] = { 0, 1, 0x8002 };

	begin_hooks();
	make_lsf(lsf, "AB1CD", 0);
	m17_lsf_encode(lsf, pay);
	emit_frame(M17_SYNC_ID_LSF, pay);
	for (int i = 0; i < 3; i++) {
		memset(pl, (uint8_t)(0xA0 + i), sizeof pl);
		emit_stream_pl(lsf, (uint8_t)i, fns[i], pl);
	}
	emit_eot();
	feed();
	CHECK(hk_start == 1, "rx_start %d", hk_start);
	CHECK(hk_voice == 3, "rx_voice %d", hk_voice);
	CHECK(hk_lost == 0, "rx_lost %d", hk_lost);
	CHECK(hk_end == 1, "rx_end %d", hk_end);
	for (int i = 0; i < hk_voice && i < 3; i++) {
		memset(pl, (uint8_t)(0xA0 + i), sizeof pl);
		CHECK(hk_fn[i] == fns[i], "frame %d number %04X", i, hk_fn[i]);
		CHECK(memcmp(hk_pl[i], pl, 16) == 0, "frame %d bytes", i);
	}
}

static void emit_packed(const uint8_t *buf, uint16_t nbytes)
{
	for (uint16_t i = 0; i < (uint16_t)(nbytes * 4); i++)
		sym[sn++] = air_sym_get(buf, i);
}

static void test_rx_packet_two_frames(void)
{
	m17_engine_t e;
	uint8_t data[40], buf[8 * 48], expect[41];
	uint16_t n;

	for (int i = 0; i < 40; i++)
		data[i] = (uint8_t)(i + 1);
	expect[0] = 0x05;
	memcpy(expect + 1, data, 40);
	engine_of(&e, M17_TX_PACKET, NULL);
	CHECK(m17_engine_tx_start(&e, 0x05, data, 40), "two frames start");
	n = pull(&e, buf, sizeof buf);
	CHECK(n == 5 * 48, "two-frame air %u", n);

	begin_hooks();
	emit_packed(buf, n);
	feed();
	CHECK(hk_npkt == 1, "rx_packet %d", hk_npkt);
	CHECK(hk_pkt_len == 41, "len %u", hk_pkt_len);
	CHECK(memcmp(hk_pkt, expect, 41) == 0, "two-frame bytes");
}

static m17_prbs9_t bert_g;

static void emit_bert(bool pre)
{
	uint8_t bits[(M17_BERT_BITS + 7) / 8];
	uint8_t pay[M17_PAYLOAD_BYTES];

	memset(bits, 0, sizeof bits);
	m17_prbs9_bits(&bert_g, bits, M17_BERT_BITS);
	m17_bert_encode_bits(bits, pay);
	emit_frame(pre ? M17_SYNC_ID_BERT_PRE : M17_SYNC_ID_BERT, pay);
}

static void test_rx_bert_count(void)
{
	begin_hooks();
	m17_prbs9_init(&bert_g);
	emit_bert(true);
	emit_bert(false);
	emit_bert(false);
	feed();
	CHECK(rx.bert.bits == 591 && rx.bert.errors == 0, "clean bits %u errors %u", rx.bert.bits, rx.bert.errors);
}

// ── The soft modem's tap, copied from airSoftmodemTest.c. Sizes fit one call; the noise run never builds a wave. ──
#define TAP_FS		240000
#define TAP_SPAN	8
#define TAP_SYMS_MAX	4000
#define TAP_WAVE_MAX	(TAP_SYMS_MAX * 50 + 8000)
#define TAP_LEAD	2000
#define TAP_LPF_HALF	120
#define NOISE_SYMBOLS	34560000u

static double tap_wave[TAP_WAVE_MAX];
static uint32_t tap_wave_n;
static uint8_t tap_air[TAP_SYMS_MAX];
static uint32_t tap_air_n;
static double tap_lpf[2 * TAP_LPF_HALF + 1];
static int16_t tap_s[TAP_WAVE_MAX / 5];
static uint32_t tap_sn;
static uint32_t tap_rng_state = 0xC0FFEE;

static uint32_t tap_rng(void)
{
	tap_rng_state ^= tap_rng_state << 13;
	tap_rng_state ^= tap_rng_state >> 17;
	tap_rng_state ^= tap_rng_state << 5;
	return tap_rng_state;
}

static double tap_gauss(void)
{
	double u = (tap_rng() % 1000000 + 1) / 1000001.0, v = (tap_rng() % 1000000) / 1000000.0;

	return sqrt(-2 * log(u)) * cos(2 * M_PI * v);
}

static void tap_pulse(double *h, int osr, uint8_t shape, double alpha)
{
	int n = 2 * TAP_SPAN * osr + 1;
	double rate = (double)TAP_FS / osr, sum = 0;

	for (int i = 0; i < n; i++) {
		double t = (double)(i - TAP_SPAN * osr) / TAP_FS, v = 0;

		if (shape == AIR_SHAPE_RRC) {
			double x = t * rate;

			if (fabs(x) < 1e-9)
				v = 1 - alpha + 4 * alpha / M_PI;
			else if (fabs(fabs(4 * alpha * x) - 1) < 1e-6)
				v = alpha / sqrt(2) * ((1 + 2 / M_PI) * sin(M_PI / (4 * alpha)) +
							(1 - 2 / M_PI) * cos(M_PI / (4 * alpha)));
			else
				v = (sin(M_PI * x * (1 - alpha)) + 4 * alpha * x * cos(M_PI * x * (1 + alpha))) /
				    (M_PI * x * (1 - (4 * alpha * x) * (4 * alpha * x)));
		} else {
			v = abs(i - TAP_SPAN * osr) * 2 < osr ? 1 : 0;
		}
		h[i] = v;
		sum += v;
	}
	for (int i = 0; i < n; i++)
		h[i] *= osr / sum;
}

static void tap_modulate(const air_profile_t *p, double ppm)
{
	static double h[2 * TAP_SPAN * 100 + 1];
	int osr = TAP_FS / p->sym_rate;

	tap_pulse(h, osr, p->shape, p->shape_param / 100.0);
	tap_wave_n = TAP_LEAD + tap_air_n * (uint32_t)osr + TAP_LEAD;
	memset(tap_wave, 0, tap_wave_n * sizeof(double));
	for (uint32_t k = 0; k < tap_air_n; k++) {
		long at = TAP_LEAD + lround(k * osr / (1 + ppm * 1e-6)) - TAP_SPAN * osr;
		double lv = air_sym_level(tap_air[k]) * (p->dev_outer_hz / 3.0);

		for (int i = 0; i <= 2 * TAP_SPAN * osr; i++)
			if (at + i >= 0 && at + i < (long)tap_wave_n)
				tap_wave[at + i] += lv * h[i];
	}
}

// ppm is the 8 kHz clock, fast when positive: samples land closer together.
static void tap_sample(uint32_t rate, uint32_t phase, double hi_hz, double counts_per_khz, double off, double sigma,
		       double ppm)
{
	uint32_t dec = TAP_FS / rate;
	double step = (double)dec / (1.0 + ppm * 1e-6);
	double sum = 0;

	for (int i = -TAP_LPF_HALF; i <= TAP_LPF_HALF; i++) {
		double x = 2 * M_PI * hi_hz / TAP_FS * i, w = 0.54 + 0.46 * cos(M_PI * i / TAP_LPF_HALF);

		tap_lpf[i + TAP_LPF_HALF] = (i ? sin(x) / x : 1) * w;
		sum += tap_lpf[i + TAP_LPF_HALF];
	}
	tap_sn = 0;
	for (double j = phase + TAP_LPF_HALF; j + TAP_LPF_HALF < tap_wave_n; j += step) {
		uint32_t jj = (uint32_t)j;
		double v = 0;

		for (int i = -TAP_LPF_HALF; i <= TAP_LPF_HALF; i++)
			v += tap_wave[jj + (uint32_t)i] * tap_lpf[i + TAP_LPF_HALF];
		v = v / sum / 1000 * counts_per_khz + off + sigma * tap_gauss();
		tap_s[tap_sn++] = (int16_t)lround(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
	(void)dec;
}

static uint8_t call_src[M17_ADDR_BYTES];
static uint8_t call_type[2];
static int call_crc;
static uint16_t call_fn[4];
static uint8_t call_pl[4][16];
static int call_nfn;
static int call_ends;

static void on_call_start(const struct m17_rx *r, void *ctx)
{
	(void)ctx;
	if (!r->crc_ok)
		return;
	memcpy(call_src, r->lsf + M17_ADDR_BYTES, M17_ADDR_BYTES);
	memcpy(call_type, r->lsf + 12, 2);
	call_crc = 1;
}

static void on_call_voice(const uint8_t voice[16], uint16_t frame_num, void *ctx)
{
	(void)ctx;
	if (call_nfn >= 4)
		return;
	memcpy(call_pl[call_nfn], voice, 16);
	call_fn[call_nfn++] = frame_num;
}

static void on_call_end(void *ctx)
{
	(void)ctx;
	call_ends++;
}

static void on_modem_chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

static void send_call(uint8_t rolloff)
{
	m17_engine_t e;
	air_profile_t prof = m17_air_mode.profile;
	uint8_t bytes[8 * 48];
	uint8_t nvoice = 0;
	uint16_t n;

	engine_of(&e, M17_TX_VOICE, voice_seq);
	e.ctx = &nvoice;
	m17_engine_tx_start(&e, 0, NULL, 0);
	n = pull(&e, bytes, 4 * 48);
	m17_engine_tx_stop(&e);
	n = (uint16_t)(n + pull(&e, bytes + n, 2 * 48));
	tap_air_n = 0;
	for (uint16_t i = 0; i < 64; i++)
		tap_air[tap_air_n++] = air_sym_get((const uint8_t *)"\x77", (uint16_t)(i & 3));
	for (uint16_t i = 0; i < (uint16_t)(n * 4); i++)
		tap_air[tap_air_n++] = air_sym_get(bytes, i);
	prof.shape_param = rolloff;
	tap_modulate(&prof, 0);
}

static void take_call(double clock_ppm)
{
	air_softmodem_t m;
	air_framer_t framer;
	m17_rx_t r;
	m17_engine_hooks_t hooks;
	uint8_t hard[M17_FRAME_SYMS];
	int8_t soft[M17_FRAME_SYMS];

	call_crc = 0;
	call_nfn = 0;
	call_ends = 0;
	memset(call_src, 0, sizeof call_src);
	memset(call_type, 0, sizeof call_type);
	memset(&hooks, 0, sizeof hooks);
	hooks.rx_start = on_call_start;
	hooks.rx_voice = on_call_voice;
	hooks.rx_end = on_call_end;
	m17_rx_init(&r, &hooks, NULL);
	air_framer_init(&framer, &m17_air_mode, &r, hard, soft, M17_FRAME_SYMS);
	tap_sample(8000, 5, 3600, 2000, 0, 0, clock_ppm);
	air_softmodem_rx_open(&m, &m17_air_mode.profile, 8000, 2000, on_modem_chunk, &framer);
	for (uint32_t at = 0; at < tap_sn;) {
		uint16_t k = (uint16_t)(tap_sn - at < 256 ? tap_sn - at : 256);

		air_softmodem_rx(&m, tap_s + at, k, (uint32_t)(((uint64_t)at * 1000000u) / 8000u));
		at += k;
	}
}

static void expect_call(const char *what)
{
	uint8_t src[M17_ADDR_BYTES];
	uint8_t pl[16];
	uint64_t a = M17_ADDR_AB1CD;
	uint16_t want[3] = { 0, 1, 0x8002 };

	for (int i = 5; i >= 0; i--) {
		src[i] = (uint8_t)a;
		a >>= 8;
	}
	CHECK(call_crc && memcmp(call_src, src, M17_ADDR_BYTES) == 0, "%s: source", what);
	CHECK(call_type[0] == 0 && call_type[1] == 0, "%s: TYPE %02X %02X", what, call_type[0], call_type[1]);
	CHECK(call_nfn == 3, "%s: %d frames", what, call_nfn);
	CHECK(call_ends == 1, "%s: ends %d", what, call_ends);
	for (int i = 0; i < call_nfn && i < 3; i++) {
		memset(pl, 0, sizeof pl);
		pl[0] = (uint8_t)i;
		CHECK(call_fn[i] == want[i], "%s: frame %d number %04X", what, i, call_fn[i]);
		CHECK(memcmp(call_pl[i], pl, 16) == 0, "%s: frame %d bytes", what, i);
	}
}

static void test_call_through_modem(void)
{
	send_call(50);
	take_call(0);
	expect_call("roll-off 0.5");
}

static void test_call_through_chip_pulse(void)
{
	send_call(20);
	take_call(0);
	expect_call("roll-off 0.2");
}

static void test_call_clock_offset(void)
{
	send_call(50);
	take_call(50);
	expect_call("+50 ppm");
	send_call(50);
	take_call(-50);
	expect_call("-50 ppm");
}

static uint32_t noise_syms;

static void on_noise_chunk(const air_chunk_t *c, void *ctx)
{
	noise_syms += c->nsyms;
	air_framer_rx(ctx, c);
}

static void test_noise_two_hours(void)
{
	air_mode_t mode = m17_air_mode;
	air_softmodem_t m;
	air_framer_t framer;
	m17_rx_t r;
	uint8_t hard[M17_FRAME_SYMS];
	int8_t soft[M17_FRAME_SYMS];
	int16_t buf[256];
	uint32_t at = 0;

	noise_syms = 0;
	tap_rng_state = 0xC0FFEE;
	m17_rx_init(&r, NULL, NULL);
	air_framer_init(&framer, &mode, &r, hard, soft, M17_FRAME_SYMS);
	air_softmodem_rx_open(&m, &m17_air_mode.profile, 8000, 0, on_noise_chunk, &framer);
	while (noise_syms < NOISE_SYMBOLS) {
		for (uint16_t i = 0; i < 256; i++)
			buf[i] = (int16_t)lround(3000 * tap_gauss());
		air_softmodem_rx(&m, buf, 256, at);
		at += 256u * 125u;
	}
	CHECK(framer.stats.frames == 0, "noise accepted %u frames in %u symbols (%u rejects)", framer.stats.frames,
	      noise_syms, framer.stats.rejects);
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
	test_stream_order();
	test_bert_order();
	test_silence_fill();
	test_packet_crc();
	test_packet_bounds();
	test_csma();
	test_bert_count();
	test_bert_prbs_recurrence();
	test_rx_hooks_call();
	test_rx_packet_two_frames();
	test_rx_bert_count();
	test_call_through_modem();
	test_call_through_chip_pulse();
	test_call_clock_offset();
	test_noise_two_hours();
	printf("m17EngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
