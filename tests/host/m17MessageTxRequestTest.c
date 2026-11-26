// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "radio/radio_m17_msg_tx.h"
#include "m17/m17.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static radio_m17_msg_tx_t s;
static char body[RADIO_M17_MSG_TX_TEXT_MAX + 2];

int main(void)
{
	memset(body, 'x', sizeof body);
	assert(!radio_m17_msg_tx_reserve(&s, 1, 0, body, 1));
	assert(!radio_m17_msg_tx_reserve(&s, 1, 0xEE6B28000000ull, body, 1));
	assert(!radio_m17_msg_tx_reserve(&s, 1, M17_ADDR_BROADCAST + 1, body, 1));
	assert(!radio_m17_msg_tx_reserve(&s, 1, 1, body, sizeof body));
	const char *bad[] = { "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82", "\x80", "a\0b" };
	const size_t lens[] = { 2, 3, 4, 2, 1, 3 };
	for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++)
		assert(!radio_m17_msg_tx_validate(bad[i], lens[i]));
	assert(!radio_m17_msg_tx_validate(NULL, 0));
	assert(!radio_m17_msg_tx_validate(body, sizeof body));
	assert(radio_m17_msg_tx_validate(body, RADIO_M17_MSG_TX_TEXT_MAX));
	assert(!radio_m17_msg_tx_reserve(&s, 0, 1, body, 1));
	uint64_t first = radio_m17_msg_tx_reserve(&s, 42, M17_ADDR_BROADCAST, body, RADIO_M17_MSG_TX_TEXT_MAX);
	assert(first && !radio_m17_msg_tx_reserve(&s, 1, 1, body, 1));
	memset(body, 'y', sizeof body);
	assert(!radio_m17_msg_tx_rollback(&s, first + 1));
	const radio_m17_msg_tx_request_t *p = radio_m17_msg_tx_consume(&s, first);
	assert(p && p->serial == first && p->generation == 42 && p->destination == M17_ADDR_BROADCAST);
	assert(p->length == RADIO_M17_MSG_TX_TEXT_MAX && p->text[0] == 'x' && p->text[820] == 'x' && !p->text[821]);
	assert(!radio_m17_msg_tx_consume(&s, first) && !radio_m17_msg_tx_rollback(&s, first));
	assert(!radio_m17_msg_tx_reserve(&s, 1, 1, body, 1));
	assert(radio_m17_msg_tx_complete(&s, first, 7));
	assert(!radio_m17_msg_tx_complete(&s, first, 8));
	assert(radio_m17_msg_tx_result(&s).serial == first && radio_m17_msg_tx_result(&s).reason == 7);
	assert(radio_m17_msg_tx_validate("\xf0\x9f\x98\x80", 4));
	uint64_t second = radio_m17_msg_tx_reserve(&s, 3, 1, "\xf0\x9f\x98\x80", 4);
	assert(second && second != first);
	assert(!radio_m17_msg_tx_consume(&s, first) && !radio_m17_msg_tx_complete(&s, first, 9));
	assert(radio_m17_msg_tx_rollback(&s, second));
	assert(radio_m17_msg_tx_result(&s).serial == first);
	s.next_serial = UINT64_MAX - 1;
	uint64_t last = radio_m17_msg_tx_reserve(&s, 4, 1, "", 0);
	assert(last == UINT64_MAX && radio_m17_msg_tx_consume(&s, last));
	assert(radio_m17_msg_tx_complete(&s, last, 3));
	assert(!radio_m17_msg_tx_reserve(&s, 4, 1, "", 0));
	assert(radio_m17_msg_tx_result(&s).serial == last && radio_m17_msg_tx_result(&s).reason == 3);
	puts("m17 message TX request: ok");
	return 0;
}
