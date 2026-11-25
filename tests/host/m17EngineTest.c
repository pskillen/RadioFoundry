// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on the framer and the bytes of a transmission (docs/spec/m17.md 4, 5.2, 5.5, 8): a sync is kept only when
// the head hook can prove the frame, a call's LSF is rebuilt from LICH chunks by their counter, and a
// transmission is a preamble, an LSF, frames and the end marker.
#include <stdio.h>
#include <string.h>

#include "air/air_framer.h"
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
	printf("m17EngineTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
