// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The idle-memory pool (radio_lend.h): what is free while an air mode other than its owner runs.
#include "radio/radio_lend.h"

#include "config.h"
#include "board.h"
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
#include "radio/radio_p25.h"
#endif
#if CONFIG_NXDN
#include "radio/radio_nxdn.h"
#endif

// P25 and NXDN contribute their own blocks when they have one. Shared blocks enter the pool through their owner.
static void take(radio_lend_t *b, uint8_t *n, void *p, size_t len)
{
	uintptr_t aligned;
	size_t skip;

	if (p == NULL || len == 0 || *n >= RADIO_LEND_MAX)
		return;
	aligned = ((uintptr_t)p + 7u) & ~(uintptr_t)7u;
	skip = (size_t)(aligned - (uintptr_t)p);
	if (len <= skip)
		return;
	b[*n].p = (void *)aligned;
	b[*n].len = len - skip;
	(*n)++;
}

static void largest_first(radio_lend_t *b, uint8_t n)
{
	for (uint8_t i = 1; i < n; i++) {
		radio_lend_t key = b[i];
		uint8_t j = i;

		while (j > 0 && b[j - 1].len < key.len) {
			b[j] = b[j - 1];
			j--;
		}
		b[j] = key;
	}
}

uint8_t radio_lend_gather(radio_lend_t out[RADIO_LEND_MAX])
{
	radio_lend_t got[RADIO_LEND_MAX];
	uint8_t n = 0;

	// The lender writes *len. Read it after the call returns: an argument list may evaluate the
	// length before the call, and the block would be dropped as empty.
	if (board.vocoder != NULL && board.vocoder->lend != NULL) {
		size_t len = 0;
		void *p = board.vocoder->lend(&len);

		take(got, &n, p, len);
	}
#if CONFIG_P25 && !defined(HAL_VOCODER_LEND_BYTES)
	{
		size_t len = 0;
		void *p = radio_p25_idle_mem(&len);

		take(got, &n, p, len);
	}
#endif
#if CONFIG_NXDN
	{
		size_t len = 0;
		void *p = radio_nxdn_idle_mem(&len);

		take(got, &n, p, len);
	}
#endif
	largest_first(got, n);
	for (uint8_t i = 0; i < n; i++)
		out[i] = got[i];
	return n;
}
