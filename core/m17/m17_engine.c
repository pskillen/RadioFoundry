// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The bytes of one M17 transmission (docs/spec/m17.md 4, 6.2). No RTOS call: the clock and the coin are arguments.
#include "m17_engine.h"

#if CONFIG_M17

#include <string.h>

#include "m17_rx.h"
#if CONFIG_M17_PACKET
#include "m17_packet.h"
#endif

enum {
	PH_PRE = 0,
	PH_LSF,
	PH_FRAMES,
	PH_EOT,
	PH_DONE,
};

#define CHUNK_BYTES (M17_FRAME_SYMS / 4)

static const air_sync_t *sync_of(uint8_t id)
{
	for (uint8_t i = 0; i < m17_air_mode.profile.nsyncs; i++)
		if (m17_air_mode.profile.sync[i].id == id)
			return &m17_air_mode.profile.sync[i];
	return NULL;
}

// The 8 symbols a frame starts with. A 24-symbol word's frame begins at frame_at, so the sync is its tail.
static void frame_sync(uint8_t id, uint8_t out[2])
{
	const air_sync_t *s = sync_of(id);
	uint8_t off = 0;

	if (s->flags & AIR_SYNC_START_ONLY)
		off = (uint8_t)(s->frame_at / 4);
	out[0] = s->syms[off];
	out[1] = s->syms[off + 1];
}

static void put_addr(uint8_t d[6], uint64_t a)
{
	for (int i = 5; i >= 0; i--) {
		d[i] = (uint8_t)a;
		a >>= 8;
	}
}

static void fill_byte(m17_engine_t *e, uint8_t b)
{
	memset(e->chunk, b, CHUNK_BYTES);
	e->len = CHUNK_BYTES;
	e->pos = 0;
}

static void fill_frame(m17_engine_t *e, const uint8_t sync[2], const uint8_t pay[M17_PAYLOAD_BYTES])
{
	e->chunk[0] = sync[0];
	e->chunk[1] = sync[1];
	memcpy(e->chunk + 2, pay, M17_PAYLOAD_BYTES);
	e->len = CHUNK_BYTES;
	e->pos = 0;
}

static void fill_eot(m17_engine_t *e)
{
	uint8_t sync[2];

	frame_sync(M17_SYNC_ID_EOT, sync);
	for (uint16_t i = 0; i < CHUNK_BYTES; i += 2) {
		e->chunk[i] = sync[0];
		e->chunk[i + 1] = sync[1];
	}
	e->len = CHUNK_BYTES;
	e->pos = 0;
}

static void build_lsf(m17_engine_t *e)
{
	uint8_t pay[M17_PAYLOAD_BYTES];
	uint8_t sync[2];
	uint16_t type = (uint16_t)((e->cfg.can << M17_TYPE_CAN_SHIFT) & M17_TYPE_CAN_MASK);

	// Encryption, META subtype and signature bits are zero in both modes.
	if (e->cfg.tx_kind == M17_TX_VOICE)
		type |= M17_TYPE_STREAM | M17_TYPE_VOICE;

	memset(e->lsf, 0, sizeof e->lsf);
	put_addr(e->lsf, e->cfg.dst);
	put_addr(e->lsf + M17_ADDR_BYTES, e->cfg.own);
	e->lsf[M17_LSF_TYPE_OFFSET] = (uint8_t)(type >> 8);
	e->lsf[M17_LSF_TYPE_OFFSET + 1] = (uint8_t)type;
	m17_lsf_encode(e->lsf, pay);
	frame_sync(M17_SYNC_ID_LSF, sync);
	fill_frame(e, sync, pay);
}

static void voice_bytes(m17_engine_t *e, uint8_t out[16])
{
	uint8_t c2[2][8];

	if (e->hooks.tx_voice && e->hooks.tx_voice(c2, e->ctx)) {
		memcpy(out, c2[0], 8);
		memcpy(out + 8, c2[1], 8);
		return;
	}
	if (e->cfg.silence) {
		memcpy(out, e->cfg.silence, 8);
		memcpy(out + 8, e->cfg.silence, 8);
		return;
	}
	memset(out, 0, 16);
}

static void build_stream(m17_engine_t *e, bool last)
{
	uint8_t voice[16];
	uint8_t pay[M17_PAYLOAD_BYTES];
	uint8_t sync[2];
	uint16_t fn = e->fn;

	if (last)
		fn = (uint16_t)(fn | 0x8000u);
	voice_bytes(e, voice);
	m17_stream_encode(e->lsf, e->lich, fn, voice, pay);
	frame_sync(M17_SYNC_ID_STREAM, sync);
	fill_frame(e, sync, pay);
	e->lich = (uint8_t)((e->lich + 1) % 6);
	e->fn = (uint16_t)((e->fn + 1) & 0x7FFF);
}

static void build_bert(m17_engine_t *e)
{
	uint8_t bits[(M17_BERT_BITS + 7) / 8];
	uint8_t pay[M17_PAYLOAD_BYTES];
	uint8_t sync[2];

	m17_prbs9_bits(&e->prbs, bits, M17_BERT_BITS);
	m17_bert_encode_bits(bits, pay);
	frame_sync(M17_SYNC_ID_BERT, sync);
	fill_frame(e, sync, pay);
}

#if CONFIG_M17_PACKET
static void build_packet_frame(m17_engine_t *e)
{
	uint8_t data[M17_PACKET_FRAME_BYTES];
	uint8_t pay[M17_PAYLOAD_BYTES];
	uint8_t sync[2];
	bool last = false;
	uint8_t valid = 0, frame_num = 0;

	m17_packet_slice(e->pkt, e->pkt_len, e->pkt_i, data, &last, &valid, &frame_num);
	m17_packet_encode(data, frame_num, last, valid, pay);
	frame_sync(M17_SYNC_ID_PACKET, sync);
	fill_frame(e, sync, pay);
	e->pkt_i++;
}
#endif

static bool build_next(m17_engine_t *e)
{
	const air_sync_t *pre;

	if (!e->active || e->phase == PH_DONE)
		return false;
	if (e->phase == PH_PRE) {
		pre = sync_of(e->cfg.tx_kind == M17_TX_BERT ? M17_SYNC_ID_BERT_PRE : M17_SYNC_ID_LSF);
		fill_byte(e, pre->syms[0]);
		e->phase = e->cfg.tx_kind == M17_TX_BERT ? PH_FRAMES : PH_LSF;
		return true;
	}
	if (e->phase == PH_LSF) {
		build_lsf(e);
		e->phase = PH_FRAMES;
		return true;
	}
	if (e->phase == PH_EOT) {
		fill_eot(e);
		e->phase = PH_DONE;
		e->active = false;
		return true;
	}
	if (e->cfg.tx_kind == M17_TX_VOICE) {
		build_stream(e, e->stop);
		if (e->stop)
			e->phase = PH_EOT;
		return true;
	}
	if (e->cfg.tx_kind == M17_TX_BERT) {
		build_bert(e);
		if (e->stop)
			e->phase = PH_EOT;
		return true;
	}
#if CONFIG_M17_PACKET
	if (e->stop && e->pkt_i > 0) {
		e->phase = PH_EOT;
		return build_next(e);
	}
	build_packet_frame(e);
	if (e->stop || e->pkt_i >= e->pkt_n)
		e->phase = PH_EOT;
	return true;
#else
	return false;
#endif
}

void m17_engine_init(m17_engine_t *e, const m17_engine_cfg_t *cfg, const m17_engine_hooks_t *hooks, void *ctx)
{
	memset(e, 0, sizeof *e);
	if (cfg)
		e->cfg = *cfg;
	if (hooks)
		e->hooks = *hooks;
	e->ctx = ctx;
}

bool m17_engine_tx_start(m17_engine_t *e, uint8_t type, const uint8_t *data, uint16_t len)
{
	if (e->cfg.tx_inhibit)
		return false;
	e->active = false;
	e->stop = false;
	e->phase = PH_PRE;
	e->len = 0;
	e->pos = 0;
	e->fn = 0;
	e->lich = 0;
	m17_prbs9_init(&e->prbs);
	if (e->cfg.tx_kind == M17_TX_PACKET) {
#if CONFIG_M17_PACKET
		if (!m17_packet_build(type, data, len, e->pkt, &e->pkt_len))
			return false;
		e->pkt_i = 0;
		e->pkt_n = (uint8_t)((e->pkt_len + M17_PACKET_FRAME_BYTES - 1) / M17_PACKET_FRAME_BYTES);
#else
		(void)type;
		(void)data;
		(void)len;
		return false;
#endif
	}
	e->active = true;
	return true;
}

void m17_engine_tx_stop(m17_engine_t *e)
{
	e->stop = true;
}

uint16_t m17_engine_tx_read(m17_engine_t *e, uint8_t *dst, uint16_t max)
{
	uint16_t n = 0;

	while (n < max) {
		uint16_t k;

		if (e->pos >= e->len && !build_next(e))
			break;
		k = (uint16_t)(e->len - e->pos);
		if (k > (uint16_t)(max - n))
			k = (uint16_t)(max - n);
		memcpy(dst + n, e->chunk + e->pos, k);
		e->pos = (uint16_t)(e->pos + k);
		n = (uint16_t)(n + k);
	}
	return n;
}

void m17_csma_init(m17_csma_t *c)
{
	c->t0 = UINT32_MAX;
	c->clear_since = UINT32_MAX;
	c->slot = UINT32_MAX;
	c->decided = 0;
}

// 1..64 is 64 values out of 256. Zero is the missed flip the test passes.
static bool csma_take(uint8_t coin)
{
	return coin >= 1 && coin <= 64;
}

m17_csma_result_t m17_csma_poll(m17_csma_t *c, uint32_t now_ms, bool locked, bool signal, uint8_t coin)
{
	bool due;

	if (c->decided)
		return (m17_csma_result_t)c->decided;
	if (c->t0 == UINT32_MAX)
		c->t0 = now_ms;
	if (locked || signal) {
		c->clear_since = UINT32_MAX;
		c->slot = UINT32_MAX;
		if (now_ms - c->t0 >= 5000) {
			c->decided = M17_CSMA_BUSY;
			return M17_CSMA_BUSY;
		}
		return M17_CSMA_WAIT;
	}
	if (c->clear_since == UINT32_MAX) {
		c->clear_since = now_ms;
		c->slot = now_ms + M17_FRAME_MS;
		return M17_CSMA_WAIT;
	}
	due = now_ms >= c->slot;
	if (due) {
		c->slot = now_ms + M17_FRAME_MS;
		if (csma_take(coin)) {
			c->decided = M17_CSMA_SEND;
			return M17_CSMA_SEND;
		}
	}
	if (now_ms - c->t0 >= 5000) {
		c->decided = M17_CSMA_BUSY;
		return M17_CSMA_BUSY;
	}
	return M17_CSMA_WAIT;
}

#endif
