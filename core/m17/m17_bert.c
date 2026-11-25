// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// BERT PRBS9 (docs/spec/m17.md 3.7, 8.1). The same shift as the frame codec's generator; that one restarts
// each frame, this one keeps the register so a transmission is one sequence.
#include "m17_bert.h"

#if CONFIG_M17

#include <string.h>

static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bit_put(uint8_t *p, uint16_t i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	if (b)
		p[i >> 3] |= m;
	else
		p[i >> 3] &= (uint8_t)~m;
}

static uint16_t step(uint16_t s)
{
	return (uint16_t)((s >> 1) | (((s >> 4) ^ s) & 1) << 8);
}

void m17_prbs9_init(m17_prbs9_t *g)
{
	g->reg = 0x1FF;
}

void m17_prbs9_bits(m17_prbs9_t *g, uint8_t *bits, uint16_t n)
{
	uint16_t s = g->reg ? g->reg : 0x1FF;

	memset(bits, 0, (size_t)(n + 7) / 8);
	for (uint16_t i = 0; i < n; i++) {
		bit_put(bits, i, (uint8_t)(s & 1));
		s = step(s);
	}
	g->reg = s;
}

void m17_bert_count_init(m17_bert_count_t *c)
{
	memset(c, 0, sizeof *c);
}

void m17_bert_count_frame(m17_bert_count_t *c, const uint8_t *bits)
{
	uint16_t s = 0;

	// The first nine outputs are the register, low bit first. Everything after is determined.
	for (uint8_t i = 0; i < 9; i++)
		s = (uint16_t)(s | (bit_get(bits, i) << i));
	for (uint16_t i = 0; i < M17_BERT_BITS; i++) {
		if (((uint8_t)(s & 1)) != bit_get(bits, i))
			c->errors++;
		s = step(s);
	}
	c->bits += M17_BERT_BITS;
	c->frames++;
}

#endif
