// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Bounded received M17 messages. The caller serializes every access.
#ifndef CORE_RADIO_RADIO_M17_MSG_H
#define CORE_RADIO_RADIO_M17_MSG_H

#include "config.h"
#if CONFIG_M17_PACKET
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "radio/radio_m17_msg_decode.h"

#define RADIO_M17_MSG_CAPACITY 4

typedef struct {
	uint32_t id;
	uint32_t timestamp;
	bool unread;
	radio_m17_msg_decoded_t message;
} radio_m17_msg_entry_t;

typedef struct {
	uint32_t received_seq; // Advances on each insertion, modulo 2^32.
	uint32_t newest_id;   // Zero when empty.
	uint8_t count;
	uint8_t unread;
} radio_m17_msg_summary_t;

// Fixed storage, suitable for static allocation and sizeof measurement. Fields are private to the store API.
typedef struct {
	radio_m17_msg_entry_t slots[RADIO_M17_MSG_CAPACITY];
	uint32_t next_id;
	uint32_t received_seq;
	uint8_t order[RADIO_M17_MSG_CAPACITY]; // Newest first, indexes into slots.
	uint8_t count;
} radio_m17_msg_store_t;

void radio_m17_msg_store_init(radio_m17_msg_store_t *store);
// Copies message into the store, evicting the oldest if full. Returns its nonzero ID.
// message must not overlap store. No pointer into store may escape the caller's lock.
uint32_t radio_m17_msg_store_add(radio_m17_msg_store_t *store, const radio_m17_msg_decoded_t *message,
		uint32_t timestamp);
void radio_m17_msg_store_summary(const radio_m17_msg_store_t *store, radio_m17_msg_summary_t *out);
// Returns false for an absent ID or out-of-range index; output is untouched in that case.
bool radio_m17_msg_store_get(const radio_m17_msg_store_t *store, uint32_t id, radio_m17_msg_entry_t *out);
bool radio_m17_msg_store_get_at(const radio_m17_msg_store_t *store, size_t index, radio_m17_msg_entry_t *out);
bool radio_m17_msg_store_read(radio_m17_msg_store_t *store, uint32_t id);
bool radio_m17_msg_store_delete(radio_m17_msg_store_t *store, uint32_t id);
#endif
#endif
