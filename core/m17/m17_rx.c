// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One received M17 call (m17_rx.h). Chunks are stored by the LICH counter, never by the frame number: the 15-bit
// number wraps after 21.8 minutes and the two drift apart (docs/spec/m17.md 5.5).
#include "m17_rx.h"

#include "config.h"

#if CONFIG_M17

#include <string.h>

static void finish(m17_rx_t *rx)
{
	bool was = rx->in_call;

	rx->in_call = false;
	rx->late = false;
	rx->crc_ok = false;
	rx->type_known = false;
	rx->bert_run = 0;
	memset(rx->have, 0, sizeof rx->have);
	memset(rx->lsf, 0, sizeof rx->lsf);
	if (was && rx->end)
		rx->end(rx->ctx);
}

void m17_rx_init(m17_rx_t *rx, void (*end)(void *ctx), void *ctx)
{
	memset(rx, 0, sizeof *rx);
	rx->end = end;
	rx->ctx = ctx;
}

void m17_rx_reset(m17_rx_t *rx)
{
	void (*end)(void *ctx) = rx->end;
	void *ctx = rx->ctx;

	memset(rx, 0, sizeof *rx);
	rx->end = end;
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
}

// counter is 0..5 and the chunk's reserved bits are already checked. A chunk 2 whose TYPE bytes differ from the
// call in hand ends it and starts the next (design: one call at a time).
void m17_rx_on_lich(m17_rx_t *rx, uint8_t counter, const uint8_t chunk[5])
{
	bool all = true;

	if (counter == 2 && rx->type_known && (chunk[2] != rx->lsf[12] || chunk[3] != rx->lsf[13]))
		finish(rx);
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
}

void m17_rx_on_lock(m17_rx_t *rx)
{
	rx->in_call = true;
}

void m17_rx_on_lost(m17_rx_t *rx)
{
	rx->bert_run = 0;
	if (rx->in_call)
		finish(rx);
}

void m17_rx_on_eot(m17_rx_t *rx)
{
	finish(rx);
}

#endif
