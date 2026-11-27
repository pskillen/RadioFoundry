// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Single-slot copied M17 message request. Callers serialize every operation.
#ifndef CORE_RADIO_RADIO_M17_MSG_TX_H
#define CORE_RADIO_RADIO_M17_MSG_TX_H

#include "config.h"
#if CONFIG_M17_PACKET
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio/radio_m17_msg_decode.h"

#define RADIO_M17_MSG_TX_TEXT_MAX RADIO_M17_MSG_TEXT_MAX

typedef struct {
	uint64_t serial;
	uint64_t destination;
	uint32_t generation;
	uint16_t length;
	char text[RADIO_M17_MSG_TX_TEXT_MAX + 1];
} radio_m17_msg_tx_request_t;

typedef enum {
	RADIO_M17_MSG_TX_SENT = 1,
	RADIO_M17_MSG_TX_BUSY,		// no clear slot before the CSMA deadline
	RADIO_M17_MSG_TX_CONTEXT,	// selected or loaded channel changed, or session stopped
	RADIO_M17_MSG_TX_GUARD,		// another radio owner or an ordinary transmit check
	RADIO_M17_MSG_TX_ADDRESS,	// unresolved source or invalid destination
	RADIO_M17_MSG_TX_FIT,		// no transmit-capable M17 session
	RADIO_M17_MSG_TX_REFUSED,	// engine preparation, board key or provider start failed
	RADIO_M17_MSG_TX_FAILED,		// abort, stall, underrun or unconfirmed drain
	RADIO_M17_MSG_TX_TIMEOUT,	// transmit time-out
} radio_m17_msg_tx_reason_t;

typedef struct {
	uint64_t serial;
	uint8_t reason;			// radio_m17_msg_tx_reason_t
} radio_m17_msg_tx_result_t;

typedef struct {
	radio_m17_msg_tx_request_t request;
	radio_m17_msg_tx_result_t result;
	uint64_t next_serial;
	uint8_t state;
} radio_m17_msg_tx_t;

// Validate before taking the store lock. Input must remain unchanged until reserve copies it.
// A true result is required before reserve; reserve does not repeat UTF-8 validation.
bool radio_m17_msg_tx_validate(const char *text, size_t length);

// Zero-initialize the store. Serialize reserve, consume, complete, rollback and result reads.
// Reserve under the short store lock, release it before nonblocking queue submission,
// and reacquire it for rollback if submission fails. No external cancellation may act
// on a reservation before queue submission returns. Input must be the unchanged bytes
// previously validated; destination and generation must be nonzero. At serial exhaustion
// reservation fails rather than reusing a stale token.
uint64_t radio_m17_msg_tx_reserve(radio_m17_msg_tx_t *store, uint32_t generation,
		uint64_t destination, const char *text, size_t length);
// Same slot, but text is handset display encoding (ASCII and ISO-8859-1). Converted in place.
uint64_t radio_m17_msg_tx_reserve_display(radio_m17_msg_tx_t *store, uint32_t generation,
		uint64_t destination, const char *text);
bool radio_m17_msg_tx_rollback(radio_m17_msg_tx_t *store, uint64_t serial);
// Pointer remains valid until completion. Owner must copy payload before completing.
const radio_m17_msg_tx_request_t *radio_m17_msg_tx_consume(radio_m17_msg_tx_t *store, uint64_t serial);
// Complete a pending or consumed request exactly once. Rollback never publishes a result.
bool radio_m17_msg_tx_complete(radio_m17_msg_tx_t *store, uint64_t serial, uint8_t reason);
// Snapshot is invalid until serial is nonzero; a later completion replaces it.
radio_m17_msg_tx_result_t radio_m17_msg_tx_result(const radio_m17_msg_tx_t *store);
#endif
#endif
