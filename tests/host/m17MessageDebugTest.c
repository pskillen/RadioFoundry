// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Debug link packet view keys and RADIO op 44 cmd 4 (host, no RTOS).
#define main radio_core_common_tests
#include "radioCoreTest.c"
#undef main

#include "debuglink/debuglink.h"
#include "debuglink/dl_fmt.h"

static void view_text(char *buf, size_t cap)
{
	dl_text_t t;

	dl_text_init(&t, buf, cap);
	debuglink_view_m17(&sm.view, &t);
}

static void test_view_packet_keys(void)
{
	char buf[768];
	radio_m17_settings_t base = { .can = 5, .can_squelch = true, .callsign = "AB2CD" };
	radio_m17_packet_rx_snap_t rx;
	radio_m17_packet_stats_t stats;

	setup_m17_talk_to();
	CHECK(radio_m17_begin(&base, 0));
	radio_m17_packet_stats(&stats);
	memset(&stats, 0, sizeof stats);
	CHECK(stats.crc_ok == 0);
	view_text(buf, sizeof buf);
	CHECK(strstr(buf, "m17.packet.built=1\n") != NULL);

	radio_m17_packet_rx_snap(&rx);
	CHECK(rx.rx_seq == 0);
	CHECK(strstr(buf, "m17.packet.tx_seq=0\n") != NULL);
	CHECK(strstr(buf, "m17.packet.tx_result=0\n") != NULL);
	radio_m17_end();
}

static uint8_t m17_op(const uint8_t *payload, unsigned n)
{
	return debuglink_radio_feature_op(DL_OP_M17, payload, (uint16_t)n);
}

static void test_msg_op_dispatch(void)
{
	uint8_t addr[M17_ADDR_BYTES];

	setup_m17_talk_to();
	CHECK(sm.view.m17_selected.generation != 0);
	CHECK(m17_addr_encode("INFO", addr));
	CHECK(m17_op((const uint8_t[]){ 4 }, 1) == DL_ST_BAD_ARG);
	CHECK(m17_op((const uint8_t[]){ 4, 0, 0, 0, 0, 0, 0 }, 7) == DL_ST_BAD_ARG);
	test_m17_msg_send_serial = 8;
	CHECK(m17_op((const uint8_t[]){ 4, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 'h', 'i' }, 9) == DL_ST_OK);
	CHECK(m17_op((const uint8_t[]){ 0, 16 }, 2) == DL_ST_BAD_ARG); // settings CAN still guarded

	test_m17_msg_send_serial = 7;
	CHECK(m17_op((const uint8_t[]){ 4, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], 'h', 'i' }, 9) ==
	      DL_ST_OK);

	sm.view.m17_selected.generation = 0;
	CHECK(m17_op((const uint8_t[]){ 4, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], 'h', 'i' }, 9) ==
	      DL_ST_REFUSED);

	test_m17_msg_send_serial = 0;
	CHECK(m17_op((const uint8_t[]){ 4, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], 'h', 'i' }, 9) ==
	      DL_ST_REFUSED);

	CHECK(m17_op((const uint8_t[]){ 2 }, 1) == DL_ST_OK); // BERT tx still works
	CHECK(m17_op((const uint8_t[]){ 3 }, 1) == DL_ST_OK);
}

int main(void)
{
	test_view_packet_keys();
	test_msg_op_dispatch();
	printf("# m17MessageDebugTest passed\n");
	return 0;
}