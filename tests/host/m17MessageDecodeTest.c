// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Packet admission and bounded UTF-8 conversion, independent of the radio task.
#include <stdio.h>
#include <string.h>
#include "config.h"
#include "radio/radio_m17_msg_decode.h"

static unsigned checks, failures;
#define CHECK(c) do { \
	checks++; \
	if (!(c)) { \
		failures++; \
		printf("FAIL %d: %s\n", __LINE__, #c); \
	} \
} while (0)

int main(void)
{
	uint8_t lsf[M17_LSF_BYTES] = {0}, own[M17_ADDR_BYTES], packet[RADIO_M17_MSG_TEXT_MAX + 4];
	radio_m17_msg_decoded_t out;
	m17_addr_encode("AB1CD", own);
	memcpy(lsf, own, sizeof own);
	m17_addr_encode("AB2CD", lsf + M17_ADDR_BYTES);
	packet[0] = 5;
	memcpy(packet + 1, "Caf\xC3\xA9", 6);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADMITTED);
	CHECK(out.text_octets == 5 && !out.cut && (unsigned char)out.text[3] == 0xE9 && !out.text[4]);
	CHECK(memcmp(out.caller, lsf + M17_ADDR_BYTES, M17_ADDR_BYTES) == 0);
	m17_addr_encode("ALL", lsf);
	CHECK(radio_m17_msg_decode(lsf, NULL, packet, 7, &out) == RADIO_M17_MSG_ADMITTED);
	memset(lsf, 0xFF, M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, NULL, packet, 7, &out) == RADIO_M17_MSG_ADMITTED);
	memset(lsf, 0, M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADDRESS);
	memcpy(lsf, own, sizeof own);
	memset(lsf + M17_ADDR_BYTES, 0xFF, M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADDRESS);
	memset(lsf + M17_ADDR_BYTES, 0, M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADDRESS);
	memset(lsf + M17_ADDR_BYTES, 0xFE, M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADDRESS);
	m17_addr_encode("ALL", lsf + M17_ADDR_BYTES);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_ADDRESS);
	m17_addr_encode("AB2CD", lsf + M17_ADDR_BYTES);
	lsf[M17_LSF_TYPE_OFFSET + 1] = M17_TYPE_STREAM;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_UNSUPPORTED_TYPE);
	lsf[M17_LSF_TYPE_OFFSET + 1] = 1u << M17_TYPE_ENC_SHIFT;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_UNSUPPORTED_TYPE);
	lsf[M17_LSF_TYPE_OFFSET + 1] = 0;
	packet[0] = 4;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 7, &out) == RADIO_M17_MSG_UNSUPPORTED_TYPE);
	packet[0] = 5;
	memset(packet + 1, 'x', RADIO_M17_MSG_TEXT_MAX + 1);
	for (unsigned n = 143; n <= 144; n++) {
		packet[n + 1] = 0;
		CHECK(radio_m17_msg_decode(lsf, own, packet, n + 2, &out) == RADIO_M17_MSG_ADMITTED);
		CHECK(out.text_octets == n && out.cut == (n > 143) && strlen(out.text) == 143);
		packet[n + 1] = 'x';
	}
	packet[822] = 0;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 823, &out) == RADIO_M17_MSG_ADMITTED && out.cut && out.text_octets == 821);
	packet[824] = 0;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 825, &out) == RADIO_M17_MSG_MALFORMED_TEXT);
	packet[200] = 0;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 823, &out) == RADIO_M17_MSG_MALFORMED_TEXT);
	packet[200] = 0xFF;
	CHECK(radio_m17_msg_decode(lsf, own, packet, 823, &out) == RADIO_M17_MSG_MALFORMED_TEXT);
	packet[200] = 'x';
	packet[822] = 'x';
	CHECK(radio_m17_msg_decode(lsf, own, packet, 823, &out) == RADIO_M17_MSG_MALFORMED_TEXT);
	// The tail is validated after the display has filled.
	static const char *const invalid[] = { "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80" };
	for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
		memset(packet + 1, 'x', 150);
		size_t n = strlen(invalid[i]);
		memcpy(packet + 151, invalid[i], n + 1);
		CHECK(radio_m17_msg_decode(lsf, own, packet, 152 + n, &out) == RADIO_M17_MSG_MALFORMED_TEXT);
	}
	memset(packet + 1, 'x', 140);
	memcpy(packet + 141, "\xE4\xB8\xAD", 4);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 145, &out) == RADIO_M17_MSG_ADMITTED);
#if CONFIG_LANG_ZH
	CHECK(!out.cut && strlen(out.text) == 143 && memcmp(out.text + 140, "\xE4\xB8\xAD", 3) == 0);
#else
	CHECK(!out.cut && strlen(out.text) == 141 && out.text[140] == '?');
#endif
	memset(packet + 1, 'x', 142);
	memcpy(packet + 143, "\xE4\xB8\xAD", 4);
	CHECK(radio_m17_msg_decode(lsf, own, packet, 147, &out) == RADIO_M17_MSG_ADMITTED);
#if CONFIG_LANG_ZH
	CHECK(out.cut && strlen(out.text) == 142);
#else
	CHECK(!out.cut && strlen(out.text) == 143 && out.text[142] == '?');
#endif
	printf("%u checks, %u failures\n", checks, failures);
	return failures != 0;
}
