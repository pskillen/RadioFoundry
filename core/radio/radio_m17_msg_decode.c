// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "radio/radio_m17_msg_decode.h"
#include "config.h"
#include <string.h>

#if CONFIG_M17 && CONFIG_M17_PACKET
#include "m17/m17_packet.h"
#include "m17/m17_text.h"

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
		uint32_t cp;
		if (!m17_text_next(packet, len - 1, &at, &cp))
			return RADIO_M17_MSG_MALFORMED_TEXT;
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
