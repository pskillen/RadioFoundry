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

static bool reserve_slot(const radio_m17_msg_tx_t *s, uint32_t generation, uint64_t destination)
{
	return s && s->state == FREE && generation && destination &&
		(destination < ADDRESS_LIMIT || destination == M17_ADDR_BROADCAST) &&
		s->next_serial != UINT64_MAX;
}

static uint64_t reserve_commit(radio_m17_msg_tx_t *s, uint32_t generation,
		uint64_t destination, size_t length)
{
	// Never reuse a token, including after exhaustion.
	++s->next_serial;
	s->request.serial = s->next_serial;
	s->request.destination = destination;
	s->request.generation = generation;
	s->request.length = (uint16_t)length;
	s->request.text[length] = '\0';
	s->state = PENDING;
	return s->request.serial;
}

uint64_t radio_m17_msg_tx_reserve(radio_m17_msg_tx_t *s, uint32_t generation,
		uint64_t destination, const char *text, size_t length)
{
	if (!reserve_slot(s, generation, destination) || !text || length > RADIO_M17_MSG_TX_TEXT_MAX)
		return 0;
	memcpy(s->request.text, text, length);
	return reserve_commit(s, generation, destination, length);
}

uint64_t radio_m17_msg_tx_reserve_display(radio_m17_msg_tx_t *s, uint32_t generation,
		uint64_t destination, const char *text)
{
	size_t n;

	// Handset text lands in the slot this store already owns. A second copy does not fit.
	if (!reserve_slot(s, generation, destination) || text == NULL)
		return 0;
	n = m17_text_from_display(text, s->request.text, sizeof s->request.text);
	if (n == 0 || n > RADIO_M17_MSG_TEXT_MAX || !radio_m17_msg_tx_validate(s->request.text, n))
		return 0;
	return reserve_commit(s, generation, destination, n);
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
