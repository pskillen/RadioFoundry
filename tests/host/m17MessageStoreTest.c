// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "radio/radio_m17_msg.h"

int main(void)
{
	radio_m17_msg_store_t store;
	radio_m17_msg_decoded_t input = {0};
	radio_m17_msg_entry_t entry = {0};
	radio_m17_msg_summary_t summary;
	uint32_t ids[5];
	radio_m17_msg_store_init(&store);
	radio_m17_msg_store_summary(&store, &summary);
	assert(summary.count == 0 && summary.unread == 0 && summary.newest_id == 0 && summary.received_seq == 0);
	assert(!radio_m17_msg_store_get(&store, 0, &entry));
	for (unsigned i = 0; i < 5; ++i) {
		input.caller[0] = (uint8_t)i;
		input.text[0] = (char)('A' + i);
		input.text_octets = (uint16_t)(100 + i);
		input.cut = i == 4;
		ids[i] = radio_m17_msg_store_add(&store, &input, 200 + i);
		assert(ids[i] != 0);
	}
	memset(&input, 0, sizeof(input)); // Store owns its copy.
	radio_m17_msg_store_summary(&store, &summary);
	assert(summary.count == 4 && summary.unread == 4 && summary.newest_id == ids[4] && summary.received_seq == 5);
	assert(!radio_m17_msg_store_get(&store, ids[0], &entry));
	for (unsigned i = 0; i < 4; ++i) {
		assert(radio_m17_msg_store_get_at(&store, i, &entry));
		assert(entry.id == ids[4 - i] && entry.timestamp == 204 - i && entry.unread);
		assert(entry.message.caller[0] == 4 - i && entry.message.text[0] == 'E' - (int)i);
		assert(entry.message.text_octets == 104 - i && entry.message.cut == (i == 0));
	}
	entry.id = 12345;
	assert(!radio_m17_msg_store_get_at(&store, 4, &entry) && entry.id == 12345);
	assert(radio_m17_msg_store_read(&store, ids[2]));
	assert(radio_m17_msg_store_read(&store, ids[2]));
	assert(!radio_m17_msg_store_read(&store, ids[0]));
	assert(radio_m17_msg_store_get(&store, ids[2], &entry) && !entry.unread);
	entry.message.text[0] = 'Z';
	assert(radio_m17_msg_store_get(&store, ids[2], &entry) && entry.message.text[0] == 'C');
	assert(radio_m17_msg_store_delete(&store, ids[3]));
	assert(!radio_m17_msg_store_delete(&store, ids[3]));
	radio_m17_msg_store_summary(&store, &summary);
	assert(summary.count == 3 && summary.unread == 2 && summary.received_seq == 5);
	assert(radio_m17_msg_store_get_at(&store, 1, &entry) && entry.id == ids[2]);
	// Exercise unsigned wrap and collisions with IDs still in the inbox.
	store.next_id = UINT32_MAX;
	uint32_t last = radio_m17_msg_store_add(&store, &input, 0);
	assert(last == UINT32_MAX);
	uint32_t wrapped = radio_m17_msg_store_add(&store, &input, 0);
	assert(wrapped != 0 && wrapped != ids[2] && wrapped != ids[4] && wrapped != last);
	assert(radio_m17_msg_store_get(&store, ids[2], &entry));
	store.next_id = ids[4];
	uint32_t collision = radio_m17_msg_store_add(&store, &input, 0);
	assert(collision != ids[4] && collision != last && collision != wrapped && collision != 0);
	assert(radio_m17_msg_store_get(&store, ids[4], &entry));
	radio_m17_msg_store_init(&store);
	radio_m17_msg_store_summary(&store, &summary);
	assert(summary.count == 0 && summary.received_seq == 0);
	printf("m17MessageStoreTest passed (store %zu bytes, entry %zu bytes)\n", sizeof(store), sizeof(entry));
	return 0;
}
