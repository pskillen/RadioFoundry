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

static void payload_of(const air_frame_t *f, uint8_t pay[M17_PAYLOAD_BYTES])
{
	memset(pay, 0, M17_PAYLOAD_BYTES);
	for (uint16_t i = 0; i < M17_PAYLOAD_BITS / 2; i++)
		air_sym_put(pay, i, air_sym_get(f->hard, (uint16_t)(M17_SYNC_SYMS + i)));
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

static bool coast_ok(const m17_rx_t *rx)
{
#if CONFIG_M17_PACKET
	if (rx->pkt_pend)
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
// A following frame one slot later: 192 symbols is 40 ms, and the chunk clock is not exact.
static bool frame_later(const air_frame_t *f, uint32_t t)
{
	uint32_t d = f->t_us - t;

	return d >= 39000 && d <= 41000;
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
static void give_packet(m17_rx_t *rx)
{
	if (!packet_crc_ok(rx))
		return;
	if (rx->hooks && rx->hooks->rx_packet)
		rx->hooks->rx_packet(rx->pkt, (uint16_t)(rx->pkt_len - 2), rx->ctx);
	rx->pkt_pend = false;
	rx->pkt_last = false;
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
	uint8_t pay[M17_PAYLOAD_BYTES];

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
	payload_of(f, pay);
	if (f->sync_id == M17_SYNC_ID_BERT) {
		// The 24-symbol BERT word counts as the first of the run; the second 8-symbol word confirms.
		if (++rx->bert_run < 2 || !bert_prbs(rx, pay)) {
			if (rx->bert_run >= 2)
				rx->bert_run = 0;
			return 0;
		}
		m17_rx_on_lock(rx);
		return M17_FRAME_SYMS;
	}
	rx->bert_run = 0;
	switch (f->sync_id) {
	case M17_SYNC_ID_LSF: {
		uint8_t lsf[M17_LSF_BYTES];

		if (m17_lsf_decode(pay, &rx->work, lsf) < 0)
			return 0;
		m17_rx_on_lsf(rx, lsf);
		return M17_FRAME_SYMS;
	}
	case M17_SYNC_ID_STREAM: {
		uint8_t counter, chunk[5];

		if (lich_ok(pay, &counter, chunk) < 0)
			return 0;
		m17_rx_on_lich(rx, counter, chunk);
		return M17_FRAME_SYMS;
	}
	case M17_SYNC_ID_PACKET: {
#if CONFIG_M17_PACKET
		uint8_t data[25], fn, vb;
		bool last;
		uint16_t n;
		bool follow;

		// The decoder always returns a byte string, so one sync is not proof. The next packet sync has to
		// land a frame later, and the CRC over the gathered bytes has to match (spec 5.2).
		if (m17_packet_decode(pay, &rx->work, data, &fn, &last, &vb) < 0)
			return 0;
		n = last ? vb : 25;
		if (n == 0 || n > 25) {
			rx->pkt_pend = false;
			rx->pkt_last = false;
			return 0;
		}
		follow = rx->pkt_pend && frame_later(f, rx->pkt_t);
		if (!follow || (!last && fn != rx->pkt_next) || (uint32_t)rx->pkt_len + n > sizeof rx->pkt) {
			if (!last && fn != 0) {
				rx->pkt_pend = false;
				rx->pkt_last = false;
				return 0;
			}
			memcpy(rx->pkt, data, n);
			rx->pkt_len = n;
			rx->pkt_next = 1;
			rx->pkt_t = f->t_us;
			rx->pkt_pend = true;
			// A one-frame packet never sees a second packet sync. The end marker is what confirms it.
			rx->pkt_last = last;
			return 0;
		}
		memcpy(rx->pkt + rx->pkt_len, data, n);
		rx->pkt_len = (uint16_t)(rx->pkt_len + n);
		rx->pkt_t = f->t_us;
		rx->pkt_next++;
		if (!last) {
			rx->pkt_last = false;
			return 0;
		}
		if (!packet_crc_ok(rx)) {
			rx->pkt_pend = false;
			rx->pkt_last = false;
			return 0;
		}
		give_packet(rx);
		m17_rx_on_lock(rx);
		return M17_FRAME_SYMS;
#else
		return 0;
#endif
	}
	case M17_SYNC_ID_BERT_PRE:
		if (!bert_prbs(rx, pay))
			return 0;
		rx->bert_run = 1;
		m17_rx_on_lock(rx);
		return M17_FRAME_SYMS;
	case M17_SYNC_ID_EOT:
		// A one-frame packet is still pending: the end marker one frame later, its second word, and the CRC.
#if CONFIG_M17_PACKET
		if (rx->pkt_pend && rx->pkt_last && frame_later(f, rx->pkt_t) && end_word_at(f, M17_SYNC_SYMS))
			give_packet(rx);
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
		if (rx->bert_run > 0)
			rx->bert.lost++;
		else if (rx->hooks && rx->hooks->rx_lost)
			rx->hooks->rx_lost(rx->ctx);
		return;
	}
	if (f->sync_id == M17_SYNC_ID_STREAM) {
		uint8_t pay[M17_PAYLOAD_BYTES];
		uint8_t lich, chunk[5], pl[16];
		uint16_t fn = 0;
		int lich_errs = 0;

		payload_of(f, pay);
		if (m17_stream_decode(pay, &rx->work, &lich, chunk, &fn, pl, &lich_errs) >= 0 && rx->hooks &&
		    rx->hooks->rx_voice)
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
			{ .id = M17_SYNC_ID_STREAM, .nsyms = 8, .max_errs = 1, .frame_at = 0,
			  .syms = { 0xFF, 0x5D } },
			{ .id = M17_SYNC_ID_LSF, .nsyms = 24, .max_errs = 2, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0x77, 0x77, 0x77, 0x77, 0x55, 0xF7 } },
			{ .id = M17_SYNC_ID_PACKET, .nsyms = 8, .max_errs = 1, .frame_at = 0,
			  .syms = { 0x75, 0xFF } },
			{ .id = M17_SYNC_ID_BERT_PRE, .nsyms = 24, .max_errs = 2, .flags = AIR_SYNC_START_ONLY,
			  .frame_at = 16, .syms = { 0xDD, 0xDD, 0xDD, 0xDD, 0xDF, 0x55 } },
			{ .id = M17_SYNC_ID_BERT, .nsyms = 8, .max_errs = 1, .frame_at = 0,
			  .syms = { 0xDF, 0x55 } },
			{ .id = M17_SYNC_ID_EOT, .nsyms = 8, .max_errs = 1, .frame_at = 0,
			  .syms = { 0x55, 0x5D } },
		},
	},
	.frame_syms = M17_FRAME_SYMS,
	.head_syms = M17_FRAME_SYMS,
	.lost_frames = M17_LOST_FRAMES,
	.head = rx_head,
	.rx_frame = rx_frame,
	.rx_lost = rx_lost,
};

#endif
