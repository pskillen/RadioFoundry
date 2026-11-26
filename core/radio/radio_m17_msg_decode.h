// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Admission and display conversion for a CRC-verified M17 packet (docs/spec/m17.md 8.2).
#ifndef CORE_RADIO_RADIO_M17_MSG_DECODE_H
#define CORE_RADIO_RADIO_M17_MSG_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "m17/m17.h"

#define RADIO_M17_MSG_DISPLAY_MAX 143
#define RADIO_M17_MSG_TEXT_MAX 821

typedef enum {
	RADIO_M17_MSG_ADMITTED,
	RADIO_M17_MSG_UNSUPPORTED_TYPE,
	RADIO_M17_MSG_ADDRESS,
	RADIO_M17_MSG_MALFORMED_TEXT,
} radio_m17_msg_result_t;

typedef struct {
	uint8_t caller[M17_ADDR_BYTES]; // Verified raw SRC, also the reply target.
	uint16_t text_octets;          // Original UTF-8 bytes, excluding the final NUL.
	bool cut;                      // Display capacity omitted at least one glyph.
	char text[RADIO_M17_MSG_DISPLAY_MAX + 1];
} radio_m17_msg_decoded_t;

// LSF integrity, CAN and packet CRC must already have passed. packet includes the type and final NUL but not CRC.
// own is the applied binary address; a non-usable own address cannot match a private destination.
// out is defined only on ADMITTED. Input and output must not overlap.
radio_m17_msg_result_t radio_m17_msg_decode(const uint8_t lsf[M17_LSF_BYTES],
		const uint8_t own[M17_ADDR_BYTES], const uint8_t *packet, size_t len, radio_m17_msg_decoded_t *out);

#endif
