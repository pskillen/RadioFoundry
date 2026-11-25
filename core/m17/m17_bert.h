// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// BERT's PRBS9 and the running error count (docs/spec/m17.md 8.1). The frame codec stays in m17_frame.c.
#ifndef RFOUNDRY_M17_BERT_H
#define RFOUNDRY_M17_BERT_H

#include <stdint.h>

#include "config.h"

#if CONFIG_M17

#include "m17.h"

// Register 0x1FF, low bit out, feedback (r ^ (r >> 4)) & 1 into bit 8. Every output bit then obeys
// b[n] = b[n-5] XOR b[n-9]. The receiver locks from the bits it got, so the fill only has to be this one.
typedef struct {
	uint16_t reg;
} m17_prbs9_t;

void m17_prbs9_init(m17_prbs9_t *g);
void m17_prbs9_bits(m17_prbs9_t *g, uint8_t *bits, uint16_t n);

// One decoded frame against the sequence locked from its first 9 bits. A later flipped bit counts once.
typedef struct {
	uint32_t bits;
	uint32_t errors;
	uint32_t frames;
} m17_bert_count_t;

void m17_bert_count_init(m17_bert_count_t *c);
void m17_bert_count_frame(m17_bert_count_t *c, const uint8_t *bits);

#endif
#endif
