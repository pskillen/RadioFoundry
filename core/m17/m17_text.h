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

// Handset text is ASCII plus ISO-8859-1 (one byte a glyph). On-air text is UTF-8.
// Writes a valid UTF-8 string, or returns 0 when it does not fit. An empty input returns 0.
static inline size_t m17_text_from_display(const char *in, char *out, size_t cap)
{
	size_t n = 0;

	if (in == NULL || out == NULL || cap == 0)
		return 0;
	for (const unsigned char *p = (const unsigned char *)in; *p != 0; p++) {
		unsigned char c = *p;
		if (c >= 0x20u && c < 0x80u) {
			if (n + 1u >= cap)
				return 0;
			out[n++] = (char)c;
		} else if (c >= 0xA0u) {
			if (n + 2u >= cap)
				return 0;
			out[n++] = (char)(0xC0u | (c >> 6));
			out[n++] = (char)(0x80u | (c & 0x3Fu));
		} else {
			if (n + 1u >= cap)
				return 0;
			out[n++] = '?';
		}
	}
	out[n] = '\0';
	return n;
}
#endif
