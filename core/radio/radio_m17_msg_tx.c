// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "radio/radio_m17_msg_tx.h"
#if CONFIG_M17_PACKET
#include "m17/m17.h"
#include "m17/m17_text.h"
#include <string.h>

enum { FREE, PENDING, IN_FLIGHT };

// 40^9: values from here to broadcast are reserved.
#define ADDRESS_LIMIT 0xEE6B28000000ull

bool radio_m17_msg_tx_validate(const char *text, size_t length)
{
	if (!text || length > RADIO_M17_MSG_TX_TEXT_MAX)
		return false;
	size_t at = 0;
	while (at < length) {
		uint32_t scalar;
		if (!m17_text_next((const uint8_t *)text, length, &at, &scalar))
			return false;
	}
	return true;
}

uint64_t radio_m17_msg_tx_reserve(radio_m17_msg_tx_t *s, uint32_t generation,
		uint64_t destination, const char *text, size_t length)
{
	if (!s || s->state != FREE || !text || length > RADIO_M17_MSG_TX_TEXT_MAX ||
	    !generation || !destination || (destination >= ADDRESS_LIMIT && destination != M17_ADDR_BROADCAST) ||
	    s->next_serial == UINT64_MAX)
		return 0;
	// Never reuse a token, including after exhaustion.
	++s->next_serial;
	s->request.serial = s->next_serial;
	s->request.destination = destination;
	s->request.generation = generation;
	s->request.length = (uint16_t)length;
	memcpy(s->request.text, text, length);
	s->request.text[length] = '\0';
	s->state = PENDING;
	return s->request.serial;
}

bool radio_m17_msg_tx_rollback(radio_m17_msg_tx_t *s, uint64_t serial)
{
	if (!s || !serial || s->state != PENDING || s->request.serial != serial)
		return false;
	s->state = FREE;
	return true;
}

const radio_m17_msg_tx_request_t *radio_m17_msg_tx_consume(radio_m17_msg_tx_t *s, uint64_t serial)
{
	if (!s || !serial || s->state != PENDING || s->request.serial != serial)
		return NULL;
	s->state = IN_FLIGHT;
	return &s->request;
}

bool radio_m17_msg_tx_complete(radio_m17_msg_tx_t *s, uint64_t serial, uint8_t reason)
{
	if (!s || !serial || s->state == FREE || s->request.serial != serial)
		return false;
	s->result.serial = serial;
	s->result.reason = reason;
	s->state = FREE;
	return true;
}

radio_m17_msg_tx_result_t radio_m17_msg_tx_result(const radio_m17_msg_tx_t *s)
{
	return s ? s->result : (radio_m17_msg_tx_result_t){0};
}
#endif
