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
	rx->late = false;
	rx->crc_ok = false;
	rx->type_known = false;
	rx->bert_run = 0;
	rx->bert_have = false;
#if CONFIG_M17_PACKET
	rx->pkt_pend = false;
#endif
	memset(rx->have, 0, sizeof rx->have);
	memset(rx->lsf, 0, sizeof rx->lsf);
	if (was)
		call_end(rx);
}

void m17_rx_init(m17_rx_t *rx, const m17_engine_hooks_t *hooks, void *ctx)
{
	memset(rx, 0, sizeof *rx);
	rx->hooks = hooks;
	rx->ctx = ctx;
}

void m17_rx_reset(m17_rx_t *rx)
{
	const m17_engine_hooks_t *hooks = rx->hooks;
	void *ctx = rx->ctx;

	memset(rx, 0, sizeof *rx);
	rx->hooks = hooks;
	rx->ctx = ctx;
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
	call_start(rx);
}

// counter is 0..5 and the chunk's reserved bits are already checked. A chunk 2 whose TYPE bytes differ from the
// call in hand ends it and starts the next (design: one call at a time).
void m17_rx_on_lich(m17_rx_t *rx, uint8_t counter, const uint8_t chunk[5])
{
	bool all = true;
	bool was, crc_was;

	if (counter == 2 && rx->type_known && (chunk[2] != rx->lsf[12] || chunk[3] != rx->lsf[13]))
		finish(rx);
	was = rx->in_call;
	crc_was = rx->crc_ok;
	memcpy(rx->lsf + (size_t)counter * 5, chunk, 5);
	rx->have[counter] = true;
	if (counter == 2) {
		rx->late = true;
		rx->type_known = true;
	}
	rx->in_call = true;
	for (uint8_t i = 0; i < 6; i++)
		all = all && rx->have[i];
	rx->crc_ok = all && m17_crc16(rx->lsf, M17_LSF_BYTES) == 0;
	if (!was)
		call_start(rx);
	// An LSF frame sets crc_ok itself. This edge is the LICH finishing the rebuild.
	if (!crc_was && rx->crc_ok)
		call_update(rx);
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
