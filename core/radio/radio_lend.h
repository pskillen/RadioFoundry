// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Memory an idle part lends while another air mode runs (docs/spec/m17.md 10). One borrower at a time: the
// radio core runs one air mode, and a lender returns nothing while it is the one running.
#ifndef RADIO_LEND_H
#define RADIO_LEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
	void *p;
	size_t len;
} radio_lend_t;

#define RADIO_LEND_MAX	3

// Up to RADIO_LEND_MAX blocks, largest first, each 8-byte aligned. A lender that returns nothing is left out.
// Radio task, while the lenders are idle, before the borrower writes the blocks.
uint8_t radio_lend_gather(radio_lend_t out[RADIO_LEND_MAX]);

#endif
