// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One received M17 call (m17_rx.h). Chunks are stored by the LICH counter, never by the frame number: the 15-bit
// number wraps after 21.8 minutes and the two drift apart (docs/spec/m17.md 5.5).
#include "m17_rx.h"

#include "config.h"

#if CONFIG_M17

#include <string.h>

static void call_end(m17_rx_t *rx)
{
	if (rx->hooks && rx->hooks->rx_end)
		rx->hooks->rx_end(rx->ctx);
}

static void call_start(m17_rx_t *rx)
{
	if (rx->hooks && rx->hooks->rx_start)
		rx->hooks->rx_start(rx, rx->ctx);
}

static void call_update(m17_rx_t *rx)
{
	if (rx->hooks && rx->hooks->rx_update)
		rx->hooks->rx_update(rx, rx->ctx);
}

static void finish(m17_rx_t *rx)
{
	bool was = rx->in_call;

	rx->in_call = false;
	rx->link = false;
	rx->late = false;
	rx->crc_ok = false;
	rx->type_known = false;
	rx->bert_run = 0;
	rx->bert_have = false;
	rx->meta_mask = 0;
	rx->meta_have = 0;
	rx->lich_next = 0;
#if CONFIG_M17_PACKET
	rx->pkt_pend = false;
	rx->pkt_last = false;
#endif
	memset(rx->have, 0, sizeof rx->have);
	memset(rx->lsf, 0, sizeof rx->lsf);
	memset(rx->meta_text, 0, sizeof rx->meta_text);
	if (was)
		call_end(rx);
}

void m17_rx_init(m17_rx_t *rx, const m17_engine_hooks_t *hooks, void *ctx)
{
	memset(rx, 0, sizeof *rx);
	rx->hooks = hooks;
	rx->ctx = ctx;
}

void m17_rx_set_can(m17_rx_t *rx, uint8_t can, bool squelch)
{
	rx->rx_can = can;
	rx->can_squelch = squelch;
}

bool m17_rx_can_passes(const m17_rx_t *rx)
{
	if (!rx->can_squelch)
		return true;
	if (!rx->type_known)
		return false;

	return ((m17_rx_type(rx) & M17_TYPE_CAN_MASK) >> M17_TYPE_CAN_SHIFT) == rx->rx_can;
}

uint16_t m17_rx_type(const m17_rx_t *rx)
{
	return (uint16_t)((rx->lsf[M17_LSF_TYPE_OFFSET] << 8) | rx->lsf[M17_LSF_TYPE_OFFSET + 1]);
}

void m17_rx_reset(m17_rx_t *rx)
{
	const m17_engine_hooks_t *hooks = rx->hooks;
	void *ctx = rx->ctx;
	uint8_t can = rx->rx_can;
	bool squelch = rx->can_squelch;

	m17_rx_init(rx, hooks, ctx);
	m17_rx_set_can(rx, can, squelch);
}

static void accept_meta(m17_rx_t *rx)
{
	const uint8_t *meta = rx->lsf + M17_LSF_META_OFFSET;
	uint16_t type = m17_rx_type(rx);

	if ((type & (M17_TYPE_ENC_MASK | M17_TYPE_SUB_MASK)) != M17_TYPE_META_TEXT)
		return;
	uint8_t control = meta[0], mask = control >> 4, block = control & 0x0f;
	if (control == 0) {
		rx->meta_have = 0;
		rx->meta_mask = 0;
		return;
	}
	if (!(type & M17_TYPE_STREAM) && control != 0x11)
		return;
	if (mask != 1 && mask != 3 && mask != 7 && mask != 15)
		return;
	if ((block & mask) == 0 || (block & (block - 1)) != 0)
		return;
	if (rx->meta_mask != mask) {
		rx->meta_have = 0;
		rx->meta_mask = mask;
	}
	unsigned offset = 0;
	for (uint8_t bit = block; bit > 1; bit >>= 1)
		offset += M17_META_TEXT_BLOCK_BYTES;
	memcpy(rx->meta_text + offset, meta + 1, M17_META_TEXT_BLOCK_BYTES);
	rx->meta_have |= block;
}

void m17_rx_on_lsf(m17_rx_t *rx, const uint8_t lsf[M17_LSF_BYTES])
{
	if (rx->in_call)
		finish(rx);
	memcpy(rx->lsf, lsf, M17_LSF_BYTES);
	for (uint8_t i = 0; i < 6; i++)
		rx->have[i] = true;
	rx->type_known = true;
	rx->late = false;
	rx->crc_ok = true;
	rx->in_call = true;
	rx->link = true;
	accept_meta(rx);
	call_start(rx);
}

// counter is 0..5 and the chunk's reserved bits are already checked. A chunk 2 whose TYPE bytes differ from the
// call in hand ends it and starts the next (design: one call at a time).
void m17_rx_on_lich(m17_rx_t *rx, uint8_t counter, const uint8_t chunk[5])
{
	const size_t offset = (size_t)counter * 5;

	if (counter == 2 && rx->type_known &&
	    memcmp(chunk + (M17_LSF_TYPE_OFFSET - offset), rx->lsf + M17_LSF_TYPE_OFFSET, 2) != 0) {
		uint8_t next = rx->lich_next;

		finish(rx);
		rx->lich_next = next;
	}
	bool was = rx->in_call;
	if (!rx->crc_ok) {
		memcpy(rx->lsf + offset, chunk, 5);
		rx->have[counter] = true;
	}
	if (counter == 2 && !rx->type_known) {
		rx->late = true;
		rx->type_known = true;
	}
	rx->in_call = true;
	rx->link = true;
	if (!was)
		call_start(rx);
	// A gap breaks assembly. Only a new chunk 0 can start another candidate.
	if (counter != 0 && counter != rx->lich_next) {
		rx->lich_next = 0;
		return;
	}
	memcpy(rx->lich + offset, chunk, 5);
	rx->lich_next = counter == M17_LICH_COUNT_MAX ? 0 : (uint8_t)(counter + 1);
	if (rx->lich_next != 0 || m17_crc16(rx->lich, M17_LSF_BYTES) != 0)
		return;
	// A new set of addresses can also be the first evidence of a new call if its LSF was missed.
	bool new_link = rx->crc_ok && memcmp(rx->lsf, rx->lich, M17_LSF_TYPE_OFFSET) != 0;
	if (new_link) {
		finish(rx);
		rx->in_call = rx->link = rx->type_known = rx->late = true;
	}
	memcpy(rx->lsf, rx->lich, sizeof rx->lsf);
	rx->crc_ok = true;
	accept_meta(rx);
	if (new_link)
		call_start(rx);
	else
		call_update(rx);
}

void m17_rx_on_gap(m17_rx_t *rx)
{
	rx->lich_next = 0;
}

void m17_rx_on_lock(m17_rx_t *rx)
{
	bool was = rx->in_call;

	rx->in_call = true;
	if (!was)
		call_start(rx);
}

void m17_rx_on_lost(m17_rx_t *rx)
{
	rx->bert_run = 0;
#if CONFIG_M17_PACKET
	// Each packet frame but the last is not yet a frame, so the framer drops its lock. The bytes stay: the next
	// packet sync, or the end marker of a one-frame packet, is what proves them. A real gap clears them in finish.
	if (rx->pkt_pend)
		return;
#endif
	if (rx->in_call)
		finish(rx);
}

void m17_rx_on_eot(m17_rx_t *rx)
{
	finish(rx);
}

#endif
