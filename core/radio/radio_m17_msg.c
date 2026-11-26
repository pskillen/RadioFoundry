// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "radio/radio_m17_msg.h"
#if CONFIG_M17_PACKET
#include <string.h>

static size_t find_id(const radio_m17_msg_store_t *store, uint32_t id)
{
	for (size_t i = 0; i < store->count; ++i)
		if (store->slots[store->order[i]].id == id && id != 0)
			return i;
	return store->count;
}

void radio_m17_msg_store_init(radio_m17_msg_store_t *store)
{
	memset(store, 0, sizeof(*store));
	store->next_id = 1;
}

uint32_t radio_m17_msg_store_add(radio_m17_msg_store_t *store, const radio_m17_msg_decoded_t *message,
		uint32_t timestamp)
{
	uint8_t slot;
	if (store->count == RADIO_M17_MSG_CAPACITY) {
		slot = store->order[--store->count];
	} else {
		// At most four slots: find one not represented in the order array.
		for (slot = 0; slot < RADIO_M17_MSG_CAPACITY; ++slot) {
			size_t i = 0;
			while (i < store->count && store->order[i] != slot)
				++i;
			if (i == store->count)
				break;
		}
	}
	uint32_t id = store->next_id;
	while (id == 0 || find_id(store, id) != store->count)
		++id;
	store->next_id = id + 1; // Unsigned wrap is intentional; zero is skipped on the next insertion.
	for (size_t i = store->count; i > 0; --i)
		store->order[i] = store->order[i - 1];
	store->order[0] = slot;
	++store->count;
	store->slots[slot].id = id;
	store->slots[slot].timestamp = timestamp;
	store->slots[slot].unread = true;
	store->slots[slot].message = *message;
	++store->received_seq;
	return id;
}

void radio_m17_msg_store_summary(const radio_m17_msg_store_t *store, radio_m17_msg_summary_t *out)
{
	out->received_seq = store->received_seq;
	out->count = store->count;
	out->newest_id = store->count ? store->slots[store->order[0]].id : 0;
	out->unread = 0;
	for (size_t i = 0; i < store->count; ++i)
		out->unread += store->slots[store->order[i]].unread ? 1 : 0;
}

bool radio_m17_msg_store_get_at(const radio_m17_msg_store_t *store, size_t index, radio_m17_msg_entry_t *out)
{
	if (index >= store->count)
		return false;
	*out = store->slots[store->order[index]];
	return true;
}

bool radio_m17_msg_store_get(const radio_m17_msg_store_t *store, uint32_t id, radio_m17_msg_entry_t *out)
{
	return radio_m17_msg_store_get_at(store, find_id(store, id), out);
}

bool radio_m17_msg_store_read(radio_m17_msg_store_t *store, uint32_t id)
{
	size_t i = find_id(store, id);
	if (i == store->count)
		return false;
	store->slots[store->order[i]].unread = false;
	return true;
}

bool radio_m17_msg_store_delete(radio_m17_msg_store_t *store, uint32_t id)
{
	size_t i = find_id(store, id);
	if (i == store->count)
		return false;
	for (; i + 1 < store->count; ++i)
		store->order[i] = store->order[i + 1];
	--store->count;
	return true;
}
#endif
