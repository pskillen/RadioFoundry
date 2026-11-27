// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "m17/m17_rx.h"

#define FRAME_BYTES (M17_FRAME_SYMS / 4)
#define CHUNK_SYMS 48
#define TYPE_SMS 0x05

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

typedef struct {
	uint8_t air[(M17_PACKET_MAX_FRAMES + 3) * FRAME_BYTES];
	uint8_t data[M17_PACKET_MAX_BYTES - 3];
	uint16_t nframes;
} packet_t;

typedef struct {
	m17_rx_t rx;
	air_framer_t fr;
	uint8_t hard[FRAME_BYTES];
	int8_t soft[M17_FRAME_SYMS];
	uint32_t t_us;
	unsigned delivered;
	uint16_t len;
	uint8_t bytes[M17_PACKET_MAX_BYTES - 2];
	uint64_t source;
} receiver_t;

static void got_packet(const uint8_t *data, uint16_t len, void *ctx)
{
	receiver_t *r = ctx;

	CHECK(len <= sizeof r->bytes);
	CHECK(r->rx.crc_ok && !r->rx.late && !(m17_rx_type(&r->rx) & M17_TYPE_STREAM));
	r->source = 0;
	for (unsigned i = M17_ADDR_BYTES; i < 2 * M17_ADDR_BYTES; i++)
		r->source = (r->source << 8) | r->rx.lsf[i];
	r->delivered++;
	r->len = len;
	memcpy(r->bytes, data, len);
}

static void begin(receiver_t *r)
{
	static const m17_engine_hooks_t hooks = { .rx_packet = got_packet };

	memset(r, 0, sizeof *r);
	m17_rx_init(&r->rx, &hooks, r);
	air_framer_init(&r->fr, &m17_air_mode, &r->rx, r->hard, r->soft, M17_FRAME_SYMS);
	r->t_us = 1000000;
}

static void make_packet(packet_t *p, uint16_t len, uint64_t source)
{
	m17_engine_cfg_t cfg = { .own = source, .dst = M17_ADDR_BROADCAST, .can = 5, .tx_kind = M17_TX_PACKET };
	m17_engine_t tx;

	for (uint16_t i = 0; i < len; i++)
		p->data[i] = (uint8_t)(source + i);
	m17_engine_init(&tx, &cfg, NULL, NULL);
	CHECK(m17_engine_tx_start(&tx, TYPE_SMS, p->data, len));
	uint16_t expected = (uint16_t)((len + 3 + M17_PACKET_FRAME_BYTES - 1) / M17_PACKET_FRAME_BYTES + 3);
	uint16_t n = m17_engine_tx_read(&tx, p->air, sizeof p->air);

	CHECK(n == expected * FRAME_BYTES);
	p->nframes = expected;
}

static uint8_t *frame(packet_t *p, unsigned i)
{
	CHECK(i < p->nframes);
	return p->air + i * FRAME_BYTES;
}

static void feed(receiver_t *r, const uint8_t *bytes, unsigned frames)
{
	for (unsigned at = 0; at < frames * M17_FRAME_SYMS; at += CHUNK_SYMS) {
		int8_t soft[CHUNK_SYMS];
		air_chunk_t c = {
			.t_us = r->t_us + at / CHUNK_SYMS * 10000u,
			.nsyms = CHUNK_SYMS,
			.hard = bytes + at / 4,
			.soft = soft,
			.sync_at = -1,
		};

		for (uint16_t i = 0; i < CHUNK_SYMS; i++)
			soft[i] = (int8_t)(air_sym_level(air_sym_get(c.hard, i)) * 32);
		air_framer_rx(&r->fr, &c);
	}
	r->t_us += frames * 40000;
}

static void expect_packet(const receiver_t *r, const packet_t *p, uint16_t len, uint64_t source, unsigned count)
{
	CHECK(r->delivered == count);
	CHECK(r->len == len + 1);
	CHECK(r->source == source);
	CHECK(r->bytes[0] == TYPE_SMS);
	CHECK(memcmp(r->bytes + 1, p->data, len) == 0);
}

static void test_delivery(void)
{
	static const uint16_t lengths[] = { 22, 61, M17_PACKET_MAX_BYTES - 3 };
	const uint64_t source = M17_ADDR_AB1CD;
	receiver_t r;
	packet_t p;

	for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
		begin(&r);
		make_packet(&p, lengths[i], source);
		// For one frame, withhold the EOT. For multiple frames, withhold the final packet frame.
		unsigned before = p.nframes - (i == 0 ? 1 : 2);

		feed(&r, p.air, before);
		CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0);
		feed(&r, frame(&p, before), 1);
		expect_packet(&r, &p, lengths[i], source, 1);
		feed(&r, frame(&p, p.nframes - 1), 1);
		feed(&r, frame(&p, p.nframes - 1), 1);
		CHECK(r.delivered == 1 && r.rx.pkt_stats.crc_ok == 1);
		CHECK(r.rx.pkt_stats.fec_samples == p.nframes - 3u);
		CHECK(r.rx.pkt_stats.fec_peak == 0 && r.rx.pkt_stats.fec_reject == 0);
	}

	make_packet(&p, 18, source + 1);
	feed(&r, p.air, p.nframes);
	expect_packet(&r, &p, 18, source + 1, 2);
}

static void test_provenance(void)
{
	receiver_t r;
	packet_t p;

	make_packet(&p, 18, M17_ADDR_AB1CD);

	begin(&r);
	feed(&r, frame(&p, 2), 2);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 0);

	begin(&r);
	uint8_t bad_lsf[FRAME_BYTES];
	uint8_t decoded[M17_LSF_BYTES];

	memcpy(bad_lsf, frame(&p, 1), sizeof bad_lsf);
	memset(bad_lsf + 2, 0, M17_PAYLOAD_BYTES);
	CHECK(m17_lsf_decode(bad_lsf + 2, &r.rx.work, decoded) < 0);
	feed(&r, p.air, 1);
	feed(&r, bad_lsf, 1);
	feed(&r, frame(&p, 2), 2);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 0);

	begin(&r);
	feed(&r, p.air, 2);
	r.t_us += 40000;
	feed(&r, frame(&p, 2), 2);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 0);

	begin(&r);
	feed(&r, p.air, 3);
	CHECK(r.rx.pkt_stats.fec_samples == 1);
	r.t_us += 40000;
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0);

	begin(&r);
	feed(&r, p.air, 3);
	CHECK(r.rx.pkt_stats.fec_samples == 1 && !air_framer_locked(&r.fr));
	// An explicit provider gap can have no timestamp jump. The later EOT is still in its slot.
	const air_chunk_t gap = { .t_us = r.t_us, .flags = AIR_CHUNK_GAP | AIR_CHUNK_NO_SIGNAL };

	air_framer_rx(&r.fr, &gap);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_fail == 0);

	begin(&r);
	feed(&r, p.air, 3);
	air_framer_rx_reset(&r.fr);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 1);

	begin(&r);
	feed(&r, p.air, 3);
	m17_rx_reset(&r.rx);
	air_framer_rx_reset(&r.fr);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 0);

	begin(&r);
	feed(&r, p.air, 3);
	make_packet(&p, 18, M17_ADDR_AB1CD + 1);
	feed(&r, p.air, p.nframes);
	expect_packet(&r, &p, 18, M17_ADDR_AB1CD + 1, 1);
}

static void test_bad_crc_and_can(void)
{
	receiver_t r;
	packet_t p;

	for (unsigned len = 18; len <= 58; len += 40) {
		begin(&r);
		make_packet(&p, (uint16_t)len, M17_ADDR_AB1CD);
		uint8_t *payload = frame(&p, p.nframes - 2) + 2;
		uint8_t data[M17_PACKET_FRAME_BYTES], fn, vb;
		bool last;

		CHECK(m17_packet_decode(payload, &r.rx.work, data, &fn, &last, &vb) == 0 && last && vb >= 2);
		data[vb - 1] ^= 1;
		m17_packet_encode(data, fn, last, vb, payload);
		feed(&r, p.air, p.nframes);
		CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_fail == 1);
		CHECK(r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.fec_reject == 0);
	}

	begin(&r);
	make_packet(&p, 18, M17_ADDR_AB1CD);
	m17_rx_set_can(&r.rx, 4, true);
	feed(&r, p.air, p.nframes);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 1 && r.rx.pkt_stats.can_reject == 1);
	CHECK(r.rx.pkt_stats.crc_fail == 0 && r.rx.pkt_stats.fec_reject == 0);
}

static void test_eot_word(void)
{
	receiver_t r;
	packet_t p;

	begin(&r);
	make_packet(&p, 18, M17_ADDR_AB1CD);
	uint8_t bad[FRAME_BYTES];

	memcpy(bad, frame(&p, 3), sizeof bad);
	bad[2] ^= 0xff;		// Preserve the sync but break the second word.
	feed(&r, p.air, 3);
	feed(&r, bad, 1);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);
}

static void test_sequence(void)
{
	receiver_t r;
	packet_t many, one;

	make_packet(&many, 80, M17_ADDR_AB1CD);
	make_packet(&one, 18, M17_ADDR_AB1CD + 1);
	for (unsigned i = 0; i < 3; i++) {
		begin(&r);
		feed(&r, many.air, 3);	// Through nonfinal frame 0.
		if (i == 0)
			r.t_us += 40000;			// Missing frame 1 before a CRC-valid final.
		else
			feed(&r, frame(&many, i == 1 ? 2 : 4), 1);	// Duplicate 0 or premature 2.
		feed(&r, frame(&one, 2), 2);	// A CRC-valid last frame and its EOT must not restart the packet.
		CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);
		feed(&r, one.air, one.nframes);
		expect_packet(&r, &one, 18, M17_ADDR_AB1CD + 1, 1);
	}
}

static void test_interleave(void)
{
	receiver_t r;
	packet_t p;
	uint8_t stream[FRAME_BYTES] = { 0xff, 0x5d };
	uint8_t bert[2 * FRAME_BYTES];
	uint8_t lsf[M17_LSF_BYTES] = { 0 };
	uint8_t voice[16] = { 0 };

	make_packet(&p, 18, M17_ADDR_AB1CD);
	lsf[M17_LSF_TYPE_OFFSET + 1] = M17_TYPE_STREAM | M17_TYPE_VOICE;
	m17_lsf_encode(lsf, stream + 2);
	memset(bert, 0xdd, FRAME_BYTES);
	bert[FRAME_BYTES] = 0xdf;
	bert[FRAME_BYTES + 1] = 0x55;
	m17_bert_encode(bert + FRAME_BYTES + 2);

	begin(&r);
	for (uint8_t i = 0; i < 6; i++) {
		m17_stream_encode(lsf, i, i, voice, stream + 2);
		feed(&r, stream, 1);
	}
	CHECK(r.rx.late && r.rx.crc_ok);
	feed(&r, frame(&p, 2), 2);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.fec_samples == 0);

	begin(&r);
	feed(&r, p.air, 3);
	feed(&r, stream, 1);
	CHECK(r.rx.link && r.rx.pkt_phase == M17_PKT_NONE);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0);

	begin(&r);
	feed(&r, p.air, 3);
	feed(&r, bert, 2);
	CHECK(r.rx.bert.frames == 1 && r.rx.pkt_phase == M17_PKT_NONE);
	feed(&r, frame(&p, 3), 1);
	CHECK(r.delivered == 0);
}

static void test_fec_limit(void)
{
	receiver_t r;
	packet_t p;

	for (uint16_t errors = 20; errors <= 21; errors++) {
		begin(&r);
		make_packet(&p, 18, M17_ADDR_AB1CD);
		uint8_t *payload = frame(&p, 2) + 2;
		uint8_t data[M17_PACKET_FRAME_BYTES], fn, vb;
		bool last;

		// Space flips in punctured convolutional order, then map them to the transmitted payload.
		for (uint16_t i = 0; i < errors; i++) {
			uint16_t bit = m17_interleave_qpp((uint16_t)(4 + 17 * i));

			air_bit_put(payload, bit, (uint8_t)!air_bit_get(payload, bit));
		}
		CHECK(m17_packet_decode(payload, &r.rx.work, data, &fn, &last, &vb) == errors);
		CHECK(last && vb == 21 && data[0] == TYPE_SMS && memcmp(data + 1, p.data, 18) == 0);
		CHECK(m17_crc16(data, 19) == (data[19] << 8 | data[20]));

		feed(&r, p.air, p.nframes);
		CHECK(r.rx.pkt_stats.fec_samples == 1 && r.rx.pkt_stats.fec_peak == errors);
		CHECK(r.rx.pkt_stats.fec_reject == (unsigned)(errors == 21));
		CHECK(r.rx.pkt_stats.crc_ok == (unsigned)(errors == 20) && r.rx.pkt_stats.crc_fail == 0);
		if (errors == 20)
			expect_packet(&r, &p, 18, M17_ADDR_AB1CD, 1);
		else
			CHECK(r.delivered == 0);
	}
}

static void modem_chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

// An ideal discriminator tap at 8 kHz, with the M17 roll-off 0.5 transmit pulse.
static double pulse(double x)
{
	if (fabs(x) < 1e-9)
		return 0.5 + 2 / M_PI;
	if (fabs(fabs(x) - 0.5) < 1e-9)
		return (1 + 2 / M_PI) / (2 * sqrt(2));
	return (sin(M_PI * x / 2) + 2 * x * cos(1.5 * M_PI * x)) / (M_PI * x * (1 - 4 * x * x));
}

static void test_modeled_tap(void)
{
	enum { LEAD = 64, TAIL = 200, BLOCK = 64 };
	receiver_t r;
	packet_t p;
	air_softmodem_t modem;
	int16_t samples[BLOCK];

	begin(&r);
	make_packet(&p, 18, M17_ADDR_AB1CD);
	int nsyms = LEAD + p.nframes * M17_FRAME_SYMS;
	unsigned count = (unsigned)((nsyms + TAIL) * 5 / 3);

	CHECK(air_softmodem_rx_open(&modem, &m17_air_mode.profile, 8000, 2000, modem_chunk, &r.fr));
	for (unsigned at = 0; at < count; at += BLOCK) {
		uint16_t n = (uint16_t)(count - at < BLOCK ? count - at : BLOCK);

		for (uint16_t i = 0; i < n; i++) {
			double t = (at + i) * 0.6;
			int mid = (int)t;
			double v = 0;

			for (int k = mid - 8; k <= mid + 8; k++) {
				if (k < 0 || k >= nsyms)
					continue;
				uint8_t d = k < LEAD ? (k & 1 ? 3 : 1) : air_sym_get(p.air, (uint16_t)(k - LEAD));

				v += air_sym_level(d) * pulse(t - k);
			}
			samples[i] = (int16_t)lround(1600 * v);
		}
		air_softmodem_rx(&modem, samples, n, r.t_us + at * 125);
	}
	expect_packet(&r, &p, 18, M17_ADDR_AB1CD, 1);
	CHECK(r.rx.pkt_stats.fec_samples == 1 && r.rx.pkt_stats.fec_reject == 0);
}

// The first chunk is the half after a hole (AIR_CHUNK_GAP). Timestamps stay on the 40 ms marks.
static void feed_gap(receiver_t *r, const uint8_t *bytes, unsigned frames)
{
	for (unsigned at = 0; at < frames * M17_FRAME_SYMS; at += CHUNK_SYMS) {
		int8_t soft[CHUNK_SYMS];
		air_chunk_t c = {
			.t_us = r->t_us + at / CHUNK_SYMS * 10000u,
			.nsyms = CHUNK_SYMS,
			.hard = bytes + at / 4,
			.soft = soft,
			.sync_at = -1,
			.flags = at == 0 ? AIR_CHUNK_GAP : 0,
		};

		for (uint16_t i = 0; i < CHUNK_SYMS; i++)
			soft[i] = (int8_t)(air_sym_level(air_sym_get(c.hard, i)) * 32);
		air_framer_rx(&r->fr, &c);
	}
	r->t_us += frames * 40000;
}

static void slip(receiver_t *r, int32_t us)
{
	r->t_us = (uint32_t)((int32_t)r->t_us + us);
}

// 40 ms ± the copy-clock allowance in m17_air.c (M17_PACKET_CLOCK_US).
static void test_clock_slip(void)
{
	static const struct {
		int32_t us;
		bool deliver;
	} slips[] = {
		{ 2000, true }, { -2000, true },
		{ 3000, true }, { -3000, true },
		{ 3001, false }, { -3001, false },
	};
	const uint64_t source = M17_ADDR_AB1CD;

	for (unsigned i = 0; i < sizeof slips / sizeof slips[0]; i++) {
		receiver_t r;
		packet_t p;

		begin(&r);
		make_packet(&p, 18, source);
		feed(&r, p.air, 2);
		slip(&r, slips[i].us);
		feed(&r, frame(&p, 2), 1);
		feed(&r, frame(&p, 3), 1);
		if (slips[i].deliver)
			expect_packet(&r, &p, 18, source, 1);
		else
			CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);

		begin(&r);
		make_packet(&p, 18, source);
		feed(&r, p.air, 3);
		slip(&r, slips[i].us);
		feed(&r, frame(&p, 3), 1);
		if (slips[i].deliver)
			expect_packet(&r, &p, 18, source, 1);
		else
			CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);
	}
}

// One 32 ms copy half dropped before the next frame, whose sync is still in the slot. A whole frame is not.
static void test_copy_half_gap(void)
{
	const uint64_t source = M17_ADDR_AB1CD;
	receiver_t r;
	packet_t one, many;

	begin(&r);
	make_packet(&one, 18, source);
	feed(&r, one.air, 2);
	feed_gap(&r, frame(&one, 2), 2);
	expect_packet(&r, &one, 18, source, 1);

	begin(&r);
	make_packet(&one, 18, source);
	feed(&r, one.air, 3);
	feed_gap(&r, frame(&one, 3), 1);
	expect_packet(&r, &one, 18, source, 1);

	begin(&r);
	make_packet(&many, 80, source);
	feed(&r, many.air, 3);
	feed_gap(&r, frame(&many, 3), 1);
	feed(&r, many.air + 4 * FRAME_BYTES, many.nframes - 4);
	expect_packet(&r, &many, 80, source, 1);

	begin(&r);
	make_packet(&one, 18, source);
	feed(&r, one.air, 2);
	slip(&r, 40000);
	feed_gap(&r, frame(&one, 2), 2);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);

	begin(&r);
	make_packet(&many, 80, source);
	feed(&r, many.air, 3);
	slip(&r, 40000);
	feed_gap(&r, frame(&many, 3), 1);
	feed(&r, many.air + 4 * FRAME_BYTES, many.nframes - 4);
	CHECK(r.delivered == 0 && r.rx.pkt_stats.crc_ok == 0 && r.rx.pkt_stats.crc_fail == 0);
}

// Discriminator noise locks the soft modem and freezes the level it took. A carrier on that
// frozen level used to miss the link setup: the 24-symbol word's step gate never opened, so the
// framer was never offered one, while 8-symbol syncs still matched in the noise. The copy's
// scale is unknown, as on a tap that does not publish counts per kHz.
static uint32_t noise_rng;

static double noise_uniform(void)
{
	noise_rng = noise_rng * 1664525u + 1013904223u;
	return (noise_rng >> 8) * (1.0 / 16777216.0);
}

static double noise_gauss(void)
{
	double u = noise_uniform();
	double v = noise_uniform();

	if (u < 1e-12)
		u = 1e-12;
	return sqrt(-2.0 * log(u)) * cos(2.0 * M_PI * v);
}

static void noise_then_packet(double noise_ratio, double sig_scale)
{
	enum { BLOCK = 64, TAIL = 80, AMP = 1600, NOISE_MAX = 8000 * 2 };
	receiver_t r;
	packet_t p;
	air_softmodem_t modem;
	int16_t samples[BLOCK];
	int nsyms, sig0, total, at = 0;

	begin(&r);
	make_packet(&p, 18, M17_ADDR_AB1CD);
	nsyms = p.nframes * M17_FRAME_SYMS;
	noise_rng = 1;
	CHECK(air_softmodem_rx_open(&modem, &m17_air_mode.profile, 8000, 0, modem_chunk, &r.fr));
	while (at < NOISE_MAX && !modem.locked) {
		for (int i = 0; i < BLOCK; i++) {
			double v = AMP * noise_ratio * noise_gauss();

			if (v > 32767)
				v = 32767;
			if (v < -32768)
				v = -32768;
			samples[i] = (int16_t)lround(v);
		}
		air_softmodem_rx(&modem, samples, BLOCK, r.t_us + (uint32_t)at * 125u);
		at += BLOCK;
	}
	CHECK(modem.locked);
	sig0 = at;
	total = sig0 + (int)((nsyms + TAIL) * 8000.0 / 4800.0);
	for (; at < total;) {
		int n = total - at < BLOCK ? total - at : BLOCK;

		for (int i = 0; i < n; i++) {
			double t = (at + i - sig0) * (4800.0 / 8000.0);
			int mid = (int)t;
			double v = 0;

			for (int k = mid - 8; k <= mid + 8; k++) {
				if (k < 0 || k >= nsyms)
					continue;
				v += air_sym_level(air_sym_get(p.air, (uint16_t)k)) * pulse(t - k);
			}
			v *= AMP * sig_scale;
			if (v > 32767)
				v = 32767;
			if (v < -32768)
				v = -32768;
			samples[i] = (int16_t)lround(v);
		}
		air_softmodem_rx(&modem, samples, (uint16_t)n, r.t_us + (uint32_t)at * 125u);
		at += n;
	}
	expect_packet(&r, &p, 18, M17_ADDR_AB1CD, 1);
}

static void test_noise_lock_releases_level(void)
{
	noise_then_packet(2.5, 1.0);
	noise_then_packet(2.5, 3.0);
	noise_then_packet(12.0, 0.2);
}

int main(void)
{
	test_delivery();
	test_provenance();
	test_bad_crc_and_can();
	test_eot_word();
	test_sequence();
	test_interleave();
	test_fec_limit();
	test_modeled_tap();
	test_clock_slip();
	test_copy_half_gap();
	test_noise_lock_releases_level();
	puts("# PASSED m17PacketRxTest");
	return EXIT_SUCCESS;
}
