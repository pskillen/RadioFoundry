// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "radio/radio_m17_msg_decode.h"
#include "config.h"
#include <string.h>

#if CONFIG_M17 && CONFIG_M17_PACKET
#include "m17/m17_packet.h"

static bool usable(const uint8_t *addr)
{
	char call[M17_ADDR_CHARS_MAX + 1];
	uint8_t encoded[M17_ADDR_BYTES];
	return m17_addr_decode(addr, call, sizeof call) && call[0] && strcmp(call, "ALL") != 0 &&
		m17_addr_encode(call, encoded) && memcmp(addr, encoded, sizeof encoded) == 0;
}

static bool addressed(const uint8_t *dst, const uint8_t *own)
{
	static const uint8_t broadcast[M17_ADDR_BYTES] = { 255, 255, 255, 255, 255, 255 };
	uint8_t all[M17_ADDR_BYTES];
	m17_addr_encode("ALL", all);
	return memcmp(dst, broadcast, sizeof broadcast) == 0 || memcmp(dst, all, sizeof all) == 0 ||
		(own && usable(own) && memcmp(dst, own, M17_ADDR_BYTES) == 0);
}

radio_m17_msg_result_t radio_m17_msg_decode(const uint8_t lsf[M17_LSF_BYTES],
		const uint8_t own[M17_ADDR_BYTES], const uint8_t *packet, size_t len, radio_m17_msg_decoded_t *out)
{
	if (!lsf || !packet || !out || len == 0)
		return RADIO_M17_MSG_MALFORMED_TEXT;
	uint16_t type = (uint16_t)((lsf[M17_LSF_TYPE_OFFSET] << 8) | lsf[M17_LSF_TYPE_OFFSET + 1]);
	if ((type & (M17_TYPE_STREAM | M17_TYPE_ENC_MASK)) || packet[0] != 0x05)
		return RADIO_M17_MSG_UNSUPPORTED_TYPE;
	if (!addressed(lsf, own) || !usable(lsf + M17_ADDR_BYTES))
		return RADIO_M17_MSG_ADDRESS;
	if (len < 2 || len > M17_PACKET_MAX_BYTES - 2 || packet[len - 1] != 0)
		return RADIO_M17_MSG_MALFORMED_TEXT;

	size_t used = 0;
	bool cut = false;
	for (size_t at = 1; at < len - 1;) {
		size_t start = at;
		uint32_t cp = packet[at++];
		unsigned tail;
		if (cp < 0x80u)
			tail = 0;
		else if (cp >= 0xC2u && cp <= 0xDFu)
			tail = 1;
		else if (cp >= 0xE0u && cp <= 0xEFu)
			tail = 2;
		else if (cp >= 0xF0u && cp <= 0xF4u)
			tail = 3;
		else
			return RADIO_M17_MSG_MALFORMED_TEXT;
		if (cp == 0 || at + tail > len - 1)
			return RADIO_M17_MSG_MALFORMED_TEXT;
		if (tail) {
			cp &= 0x7Fu >> (tail + 1);
			for (unsigned i = 0; i < tail; i++) {
				uint8_t b = packet[at++];
				if ((b & 0xC0u) != 0x80u)
					return RADIO_M17_MSG_MALFORMED_TEXT;
				cp = (cp << 6) | (b & 0x3Fu);
			}
			if (cp < (tail == 1 ? 0x80u : tail == 2 ? 0x800u : 0x10000u) ||
			    (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
				return RADIO_M17_MSG_MALFORMED_TEXT;
		}
		unsigned n = CONFIG_LANG_ZH && cp >= 0x800u && cp <= 0xFFFFu ? 3u : 1u;
		if (cut || used + n > RADIO_M17_MSG_DISPLAY_MAX) {
			cut = true;
			continue;
		}
		if (n == 3) {
			memcpy(out->text + used, packet + start, 3);
		} else {
			out->text[used] = (cp >= 0x20u && cp <= 0x7Eu) || (cp >= 0xA0u && cp <= 0xFFu)
				? (char)cp : '?';
		}
		used += n;
	}
	out->text[used] = '\0';
	out->text_octets = (uint16_t)(len - 2);
	out->cut = cut;
	memcpy(out->caller, lsf + M17_ADDR_BYTES, M17_ADDR_BYTES);
	return RADIO_M17_MSG_ADMITTED;
}

#endif // CONFIG_M17 && CONFIG_M17_PACKET
