// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Strict UTF-8 scalar iterator for length-delimited M17 text, excluding NUL.
#ifndef CORE_M17_M17_TEXT_H
#define CORE_M17_M17_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// On success advances *at and returns a nonzero Unicode scalar in *scalar.
static inline bool m17_text_next(const uint8_t *text, size_t length, size_t *at, uint32_t *scalar)
{
	if (*at >= length)
		return false;
	size_t pos = *at;
	uint32_t cp = text[pos++];
	unsigned tail;
	if (cp == 0)
		return false;
	if (cp < 0x80u)
		tail = 0;
	else if (cp >= 0xC2u && cp <= 0xDFu)
		tail = 1;
	else if (cp >= 0xE0u && cp <= 0xEFu)
		tail = 2;
	else if (cp >= 0xF0u && cp <= 0xF4u)
		tail = 3;
	else
		return false;
	if (tail > length - pos)
		return false;
	if (tail) {
		cp &= 0x7Fu >> (tail + 1);
		for (unsigned i = 0; i < tail; i++) {
			uint8_t b = text[pos++];
			if ((b & 0xC0u) != 0x80u)
				return false;
			cp = (cp << 6) | (b & 0x3Fu);
		}
		if (cp < (tail == 1 ? 0x80u : tail == 2 ? 0x800u : 0x10000u) ||
		    (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
			return false;
	}
	*at = pos;
	*scalar = cp;
	return true;
}
#endif
