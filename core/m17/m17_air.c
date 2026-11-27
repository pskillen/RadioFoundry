// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 on the air interface (docs/spec/m17.md 1.1, 5.2): the profile, the sync words, and the head hook that
// confirms a sync before the framer keeps the frame. An 8-symbol word with one error allowed also turns up in
// payload and noise, so the hook accepts only on the proof in 5.2.
#include "m17_rx.h"

#include "config.h"

#if CONFIG_M17

#include <string.h>

#include "../air/air_syms.h"
#include "../fec/golay.h"
#include "../../hal/hal_radio.h"

// How many frame-starts with no sync drop the lock when the head hook was not asked. While a stream call or a
// BERT run is on, a frame with no sync is kept: the framer is coasting (spec 5.5) and the frame hook reports it.
// Outside a call the hook still refuses it, and the signal ends there.
#define M17_LOST_FRAMES		5

// Every M17 frame has a byte-aligned payload behind the eight-symbol sync.
static const uint8_t *payload_of(const air_frame_t *f)
{
	return f->hard + M17_SYNC_SYMS / 4;
}

// 0 when every Golay word was corrected in at most 2 bits, the counter is 0..5 and the reserved bits are 0.
// fec_golay24_decode returns 0-3 or -1; 3 and -1 are not a word we trust (distance 8: two corrections leave the
// word at least six bits from every other codeword, so there is no runner-up check).
static int lich_ok(const uint8_t pay[M17_PAYLOAD_BYTES], uint8_t *counter, uint8_t chunk[5])
{
	uint32_t words[4];
	uint8_t bits[6];

	m17_stream_lich_words(pay, words);
	memset(bits, 0, sizeof bits);
	for (uint8_t w = 0; w < 4; w++) {
		uint16_t data;
		int e = fec_golay24_decode(words[w], &data);

		if (e < 0 || e > 2)
			return -1;
		for (uint8_t i = 0; i < 12; i++)
			air_bit_put(bits, (uint16_t)(w * 12 + i), (uint8_t)((data >> (11 - i)) & 1));
	}
	*counter = 0;
	for (uint8_t i = 0; i < 3; i++)
		*counter = (uint8_t)((*counter << 1) | air_bit_get(bits, (uint16_t)(40 + i)));
	if (*counter > M17_LICH_COUNT_MAX)
		return -1;
	for (uint8_t i = 0; i < 5; i++)
		if (air_bit_get(bits, (uint16_t)(43 + i)))
			return -1;
	memset(chunk, 0, 5);
	for (uint16_t i = 0; i < 40; i++)
		air_bit_put(chunk, i, air_bit_get(bits, i));
	return 0;
}

// The 24-symbol BERT word also correlates, two symbols off, inside an LSF preamble (both are a long run of
// alternating outer symbols). The spec's proof that it is BERT is the payload: 197 bits of PRBS9, each bit
// b[n] = b[n-5] XOR b[n-9] (spec 5.2, 3.7).
// The decoded bits stay on rx. The frame hook counts them; a second Viterbi decode would not be the same work.
static bool bert_prbs(m17_rx_t *rx, const uint8_t pay[M17_PAYLOAD_BYTES])
{
	rx->bert_have = false;
	if (m17_bert_decode(pay, &rx->work, rx->bert_bits) < 0)
		return false;
	for (uint16_t i = 9; i < M17_BERT_BITS; i++) {
		uint8_t exp = (uint8_t)(air_bit_get(rx->bert_bits, (uint16_t)(i - 5)) ^
					air_bit_get(rx->bert_bits, (uint16_t)(i - 9)));

		if (air_bit_get(rx->bert_bits, i) != exp)
			return false;
	}
	rx->bert_have = true;
	return true;
}

static uint16_t bert_lock(m17_rx_t *rx, uint8_t run)
{
#if CONFIG_M17_PACKET
	m17_rx_packet_end(rx);
#endif
	rx->bert_run = run;
	rx->bert_have = true;
	m17_rx_on_lock(rx);
	return M17_FRAME_SYMS;
}

static bool coast_ok(const m17_rx_t *rx)
{
#if CONFIG_M17_PACKET
	if (rx->pkt_phase != M17_PKT_NONE)
		return false;
#endif
	return rx->bert_run > 0 || rx->in_call;
}

// A preamble is one outer symbol and its negation, for the whole frame. That is a new transmission's lead-in,
// not a gap in the call: the link-setup sync is start-only, so it is found only after the lock drops.
static bool preamble_run(const air_frame_t *f)
{
	uint8_t a = air_sym_get(f->hard, 0);
	uint8_t b = air_sym_get(f->hard, 1);

	if (a == b || f->nsyms < 2)
		return false;
	for (uint16_t i = 2; i < f->nsyms; i++)
		if (air_sym_get(f->hard, i) != ((i & 1) ? b : a))
			return false;
	return true;
}

#if CONFIG_M17_PACKET
#define M17_PACKET_FEC_LIMIT	20

// 192 symbols is 40 ms. A provider publishes how far t_us can sit from the sample it names
// (hal_air_t.time_accuracy_us): 3000 µs and 2000 µs. The slot uses the wider of those. Core has no
// provider pointer, and a field to carry the number would cost RAM, so it is one constant. A frame
// a whole slot late is not a slip of that clock. A sync earlier than the window is a replay.
#define M17_PACKET_SLOT_US	40000
#define M17_PACKET_CLOCK_US	3000

static bool packet_slot(m17_rx_t *rx, const air_frame_t *f)
{
	int32_t d = (int32_t)(f->t_us - rx->pkt_t);
	int32_t late = M17_PACKET_SLOT_US + M17_PACKET_CLOCK_US;

	if (d > late)
		m17_rx_packet_end(rx);
	return d >= M17_PACKET_SLOT_US - M17_PACKET_CLOCK_US && d <= late;
}

static bool packet_crc_ok(const m17_rx_t *rx)
{
	uint16_t got;

	if (rx->pkt_len < 3)
		return false;
	got = (uint16_t)((rx->pkt[rx->pkt_len - 2] << 8) | rx->pkt[rx->pkt_len - 1]);
	return m17_crc16(rx->pkt, (uint16_t)(rx->pkt_len - 2)) == got;
}

// Type byte first, the two CRC bytes removed. The buffer stays until the caller has read it.
static bool give_packet(m17_rx_t *rx)
{
	if (!packet_crc_ok(rx)) {
		rx->pkt_stats.crc_fail++;
		m17_rx_packet_end(rx);
		return false;
	}
	rx->pkt_phase = M17_PKT_NONE;
	rx->pkt_stats.crc_ok++;
	if (!m17_rx_can_passes(rx))
		rx->pkt_stats.can_reject++;
	else if (rx->hooks && rx->hooks->rx_packet)
		rx->hooks->rx_packet(rx->pkt, (uint16_t)(rx->pkt_len - 2), rx->ctx);
	return true;
}

static uint16_t packet_head(m17_rx_t *rx, const air_frame_t *f, const uint8_t pay[M17_PAYLOAD_BYTES])
{
	uint8_t data[M17_PACKET_FRAME_BYTES], fn, vb;
	bool last;

	if (rx->pkt_phase == M17_PKT_NONE || !packet_slot(rx, f))
		return 0;

	int errs = m17_packet_decode(pay, &rx->work, data, &fn, &last, &vb);

	if (errs >= 0) {
		rx->pkt_stats.fec_samples++;
		if (errs > rx->pkt_stats.fec_peak)
			rx->pkt_stats.fec_peak = (uint16_t)errs;
	}
	if (errs < 0 || errs > M17_PACKET_FEC_LIMIT) {
		rx->pkt_stats.fec_reject++;
		m17_rx_packet_end(rx);
		return 0;
	}

	uint16_t n = last ? vb : M17_PACKET_FRAME_BYTES;
	bool first = rx->pkt_phase == M17_PKT_LSF;

	if (rx->pkt_phase == M17_PKT_ONE || (!last && fn != rx->pkt_next) ||
	    n == 0 || n > M17_PACKET_FRAME_BYTES || (uint32_t)rx->pkt_len + n > sizeof rx->pkt) {
		m17_rx_packet_end(rx);
		return 0;
	}
	memcpy(rx->pkt + rx->pkt_len, data, n);
	rx->pkt_len = (uint16_t)(rx->pkt_len + n);
	rx->pkt_t = f->t_us;
	rx->pkt_next++;
	if (!last) {
		rx->pkt_phase = M17_PKT_MORE;
		return 0;
	}
	if (first) {
		rx->pkt_phase = M17_PKT_ONE;
		return 0;
	}
	return give_packet(rx) ? M17_FRAME_SYMS : 0;
}
#endif

// The end marker is the 8-symbol word repeated. The sync that locked us is the first; the second starts at
// symbol 8. One error, the same allowance as the sync word.
static bool end_word_at(const air_frame_t *f, uint16_t at)
{
	static const uint8_t eot[2] = { 0x55, 0x5D };
	uint8_t errs = 0;

	if ((uint16_t)(at + M17_SYNC_SYMS) > f->nsyms)
		return false;
	for (uint8_t i = 0; i < M17_SYNC_SYMS; i++)
		if (air_sym_get(f->hard, (uint16_t)(at + i)) != air_sym_get(eot, i))
			errs++;
	return errs <= 1;
}

static uint16_t rx_head(const air_frame_t *f, void *ctx)
{
	m17_rx_t *rx = ctx;

	if (f->nsyms < M17_FRAME_SYMS) {
		rx->bert_run = 0;
		return 0;
	}
	// Placed by counting. A stream call or a BERT run keeps a gap; a preamble, or anything outside a call, does not.
	if (f->flags & AIR_FRAME_NO_SYNC) {
		if (!preamble_run(f) && coast_ok(rx))
			return M17_FRAME_SYMS;
		rx->bert_run = 0;
		return 0;
	}
	const uint8_t *pay = payload_of(f);

	if (f->sync_id == M17_SYNC_ID_BERT) {
		// The 24-symbol BERT word counts as the first of the run; the second 8-symbol word confirms.
		if (++rx->bert_run < 2 || !bert_prbs(rx, pay)) {
			if (rx->bert_run >= 2)
				rx->bert_run = 0;
			return 0;
		}
		return bert_lock(rx, rx->bert_run);
	}
	rx->bert_run = 0;
	switch (f->sync_id) {
	case M17_SYNC_ID_LSF: {
		uint8_t lsf[M17_LSF_BYTES];

		if (m17_lsf_decode(pay, &rx->work, lsf) < 0)
			return 0;
		m17_rx_on_lsf(rx, lsf);
#if CONFIG_M17_PACKET
		if (!(m17_rx_type(rx) & M17_TYPE_STREAM)) {
			rx->pkt_phase = M17_PKT_LSF;
			rx->pkt_t = f->t_us;
			rx->pkt_len = 0;
			rx->pkt_next = 0;
		}
#endif
		return M17_FRAME_SYMS;
	}
	case M17_SYNC_ID_STREAM: {
		uint8_t counter, chunk[5];

		if (lich_ok(pay, &counter, chunk) < 0)
			return 0;
#if CONFIG_M17_PACKET
		// Stream takeover invalidates packet provenance without ending the current call.
		rx->pkt_phase = M17_PKT_NONE;
#endif
		m17_rx_on_lich(rx, counter, chunk);
		return M17_FRAME_SYMS;
	}
	case M17_SYNC_ID_PACKET:
#if CONFIG_M17_PACKET
		return packet_head(rx, f, pay);
#else
		return 0;
#endif
	case M17_SYNC_ID_BERT_PRE:
		if (!bert_prbs(rx, pay))
			return 0;
		return bert_lock(rx, 1);
	case M17_SYNC_ID_EOT:
#if CONFIG_M17_PACKET
		if (rx->pkt_phase != M17_PKT_NONE) {
			if (!packet_slot(rx, f))
				return 0;
			if (!end_word_at(f, M17_SYNC_SYMS)) {
				m17_rx_packet_end(rx);
				return 0;
			}
			if (rx->pkt_phase == M17_PKT_ONE && !give_packet(rx))
				return 0;
		}
#endif
		// Only while a call is locked, and only when the next word is an end word too.
		if (!rx->in_call || !end_word_at(f, M17_SYNC_SYMS))
			return 0;
		m17_rx_on_eot(rx);
		return M17_FRAME_SYMS;
	default:
		return 0;
	}
}

static void rx_frame(const air_frame_t *f, void *ctx)
{
	m17_rx_t *rx = ctx;

	if (f->flags & AIR_FRAME_NO_SYNC) {
		m17_rx_on_gap(rx);
		if (rx->bert_run > 0)
			rx->bert.lost++;
		else if (rx->hooks && rx->hooks->rx_lost && m17_rx_can_passes(rx))
			rx->hooks->rx_lost(rx->ctx);
		return;
	}
	if (f->sync_id == M17_SYNC_ID_STREAM) {
		uint8_t lich, chunk[5], pl[16];
		uint16_t fn = 0;
		int lich_errs = 0;

		if (!rx->hooks || !rx->hooks->rx_voice || !m17_rx_can_passes(rx))
			return;
		if (m17_stream_decode(payload_of(f), &rx->work, &lich, chunk, &fn, pl, &lich_errs) >= 0)
			rx->hooks->rx_voice(pl, fn, rx->ctx);
		return;
	}
	if ((f->sync_id == M17_SYNC_ID_BERT || f->sync_id == M17_SYNC_ID_BERT_PRE) && rx->bert_have) {
		m17_bert_count_frame(&rx->bert, rx->bert_bits);
		rx->bert_have = false;
	}
}

static void rx_lost(void *ctx)
{
	m17_rx_on_lost(ctx);
}

#if CONFIG_M17_PACKET
static void rx_discontinuity(void *ctx, uint8_t flags)
{
	// The chunk after a missing copy half is AIR_CHUNK_GAP and can still hold the next frame.
	// The slot window takes that frame or drops it. Squelch, and a reset (no flags), have no
	// such frame on this clock.
	if ((flags & AIR_CHUNK_GAP) != 0 && (flags & AIR_CHUNK_NO_SIGNAL) == 0)
		return;
	m17_rx_packet_end(ctx);
}
#endif

// The final eight dibits of each pattern in profile order. A prefix of a start-only word is
// checked later by the framer. A hard dibit error also covers a sign error, so both inputs
// can use this table. Build it from the same bytes and error allowances as the profile.
#define STREAM_WORD	0xFF, 0x5D
#define LSF_WORD	0x55, 0xF7
#define PACKET_WORD	0x75, 0xFF
#define BERT_WORD	0xDF, 0x55
#define EOT_WORD	0x55, 0x5D
#define SHORT_ERRS	1
#define START_ERRS	2
#define WORD_SIGNS(hi, lo) \
	(((hi) & 0x80) | ((hi) & 0x20) << 1 | ((hi) & 0x08) << 2 | ((hi) & 0x02) << 3 \
	 | ((lo) & 0x80) >> 4 | ((lo) & 0x20) >> 3 | ((lo) & 0x08) >> 2 | ((lo) & 0x02) >> 1)
#define TAIL_MATCH(x, hi, lo, errs) (__builtin_popcount((x) ^ WORD_SIGNS(hi, lo)) <= (errs))
#define WORD_MATCH(x, word, errs) TAIL_MATCH(x, word, errs)
#define TAIL_MASK(x) \
	(WORD_MATCH(x, STREAM_WORD, SHORT_ERRS) | WORD_MATCH(x, LSF_WORD, START_ERRS) << 1 \
	 | WORD_MATCH(x, PACKET_WORD, SHORT_ERRS) << 2 | WORD_MATCH(x, BERT_WORD, START_ERRS) << 3 \
	 | WORD_MATCH(x, BERT_WORD, SHORT_ERRS) << 4 | WORD_MATCH(x, EOT_WORD, SHORT_ERRS) << 5)
#define TAIL_ROW(x) \
	TAIL_MASK(x), TAIL_MASK((x) + 1), TAIL_MASK((x) + 2), TAIL_MASK((x) + 3), \
	TAIL_MASK((x) + 4), TAIL_MASK((x) + 5), TAIL_MASK((x) + 6), TAIL_MASK((x) + 7), \
	TAIL_MASK((x) + 8), TAIL_MASK((x) + 9), TAIL_MASK((x) + 10), TAIL_MASK((x) + 11), \
	TAIL_MASK((x) + 12), TAIL_MASK((x) + 13), TAIL_MASK((x) + 14), TAIL_MASK((x) + 15)

static const uint8_t sync_tail_masks[256] = {
	TAIL_ROW(0x00), TAIL_ROW(0x10), TAIL_ROW(0x20), TAIL_ROW(0x30),
	TAIL_ROW(0x40), TAIL_ROW(0x50), TAIL_ROW(0x60), TAIL_ROW(0x70),
	TAIL_ROW(0x80), TAIL_ROW(0x90), TAIL_ROW(0xA0), TAIL_ROW(0xB0),
	TAIL_ROW(0xC0), TAIL_ROW(0xD0), TAIL_ROW(0xE0), TAIL_ROW(0xF0),
};

const air_mode_t m17_air_mode = {
	.name = "M17",
	.profile = {
		.sym_rate = 4800,
		.levels = 4,
		.shape = AIR_SHAPE_RRC,
		.shape_param = 50,
		.dev_outer_hz = 2400,
		.timing = AIR_TIMING_CONTINUOUS,
		.bw = RADIO_BW_12K5,
		.sync_every_syms = 0,
		.sync_any_syms = M17_FRAME_SYMS,
		.sync_period_syms = M17_FRAME_SYMS,
		.id = AIR_PROFILE_M17,
		.idle = 0x77,
		.nsyncs = 6,
		.sync = {
			{ .id = M17_SYNC_ID_STREAM, .nsyms = 8, .max_errs = SHORT_ERRS, .frame_at = 0,
			  .syms = { STREAM_WORD } },
			{ .id = M17_SYNC_ID_LSF, .nsyms = 24, .max_errs = START_ERRS, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0x77, 0x77, 0x77, 0x77, LSF_WORD } },
			{ .id = M17_SYNC_ID_PACKET, .nsyms = 8, .max_errs = SHORT_ERRS, .frame_at = 0,
			  .syms = { PACKET_WORD } },
			{ .id = M17_SYNC_ID_BERT_PRE, .nsyms = 24, .max_errs = START_ERRS, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0xDD, 0xDD, 0xDD, 0xDD, BERT_WORD } },
			{ .id = M17_SYNC_ID_BERT, .nsyms = 8, .max_errs = SHORT_ERRS, .frame_at = 0,
			  .syms = { BERT_WORD } },
			{ .id = M17_SYNC_ID_EOT, .nsyms = 8, .max_errs = SHORT_ERRS, .frame_at = 0,
			  .syms = { EOT_WORD } },
		},
	},
	.frame_syms = M17_FRAME_SYMS,
	.head_syms = M17_FRAME_SYMS,
	.lost_frames = M17_LOST_FRAMES,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
	.sync_tail_masks = sync_tail_masks,
#if CONFIG_M17_PACKET
	.rx_discontinuity = rx_discontinuity,
#endif
};

#endif
