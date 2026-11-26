// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Drive the real radio core and M17 adapter with the existing fake board and radio clock.
#define main radio_core_common_tests
#include "radioCoreTest.c"
#undef main

static radio_m17_msg_tx_request_t message;
static uint64_t next_serial;
static uint64_t completed_serial;
static uint8_t completed_reason;
static unsigned completions;

static void packet_complete(uint64_t serial, uint8_t reason)
{
	CHECK(!hw.keyed && sm.m17_msg.request == NULL);
	completed_serial = serial;
	completed_reason = reason;
	completions++;
	memset(&message, 0, sizeof message);	// the mailbox can be reused immediately
}

static void packet_submit(uint64_t dst, uint32_t generation)
{
	message = (radio_m17_msg_tx_request_t){
		.serial = ++next_serial, .destination = dst, .generation = generation, .length = 2, .text = "hi",
	};
	completions = 0;
	radio_sm_m17_msg_accept(&sm, &message, packet_complete, now);
}

static void packet_result(uint8_t reason)
{
	CHECK(completions == 1 && completed_serial == next_serial && completed_reason == reason);
}

static void packet_poll(uint32_t delta)
{
	now += delta;
	sm.m17_msg_rng = 64;	// xorshift32 yields a winning byte of 1 at a due slot
	radio_sm_poll(&sm, now);
	CHECK(!hw.keyed);	// the poll may grant a slot, but never keys
}

static void packet_tick(uint32_t delta)
{
	now += delta;
	radio_sm_tick(&sm, now);
}

static void packet_setup(void)
{
	setup_m17_talk_to();
	CHECK(sm.view.ch.mode == RADIO_MODE_M17 && radio_m17_running());
}

static void packet_ready(void)
{
	packet_poll(50);
	packet_poll(RADIO_POLL_MS);
	packet_poll(RADIO_POLL_MS);
	CHECK(completions == 0);
}

static void packet_start(void)
{
	packet_submit(m17_destination("INFO"), sm.view.m17_selected.generation);
	packet_ready();
	packet_tick(5);
	CHECK(hw.keyed && fair[0].on && sm.view.state == RS_TX && completions == 0);
}

static void packet_provider_done(uint8_t why, bool underrun)
{
	fake_air_t *p = &fair[0];
	CHECK(hw.keyed && p->on);
	air_ev(p, AIR_EV_TX_STARTED, NULL);
	if (underrun)
		air_ev(p, AIR_EV_TX_UNDERRUN, NULL);
	if (why == AIR_TX_END_DRAINED) {
		bool last = false;
		for (unsigned i = 0; i < 16 && p->qn != 0; i++) {
			last = (p->q[0].flags & AIR_TX_LAST) != 0;
			memmove(p->q, p->q + 1, (size_t)(--p->qn) * sizeof(p->q[0]));
			if (last)
				break;
			air_ev(p, AIR_EV_TX_SPACE, NULL);
		}
		CHECK(last && p->qn == 0);
	}
	p->on = false;
	p->qn = 0;
	const air_event_t end = { .kind = AIR_EV_TX_DONE, .arg = why };
	p->cb(&end, p->ctx);
	CHECK(completions == 0 && hw.keyed);
	packet_tick(1);
}

static void test_packet_carrier(void)
{
	packet_setup();
	packet_submit(m17_destination("INFO"), sm.view.m17_selected.generation);

	// Each raw indication independently vetoes what would otherwise be a winning slot.
	for (unsigned i = 0; i < 4; i++) {
		rx_signal(120, true);
		hw.st.carrier = i == 0;
		if (i == 1)
			hw.st.rssi_dbm = -99;
		if (i == 2)
			hw.st.rssi_dbm = 0;
		if (i == 3)
			hw.st.noise = UINT8_MAX;
		for (unsigned j = 0; j < 3; j++) {
			packet_poll(RADIO_POLL_MS);
			packet_tick(0);
			CHECK(!hw.keyed && completions == 0);
		}
	}
	rx_signal(120, true);
	packet_poll(RADIO_POLL_MS);
	packet_poll(RADIO_POLL_MS);
	packet_poll(RADIO_POLL_MS + 1);	// late poll begins a new interval
	packet_poll(RADIO_POLL_MS);
	packet_tick(0);
	CHECK(!hw.keyed);
	packet_poll(RADIO_POLL_MS);	// clear again, but the grant expires before a tick observes it
	packet_tick(RADIO_POLL_MS);
	CHECK(!hw.keyed);
	packet_tick(5000);		// no new status is needed for the deadline
	packet_result(RADIO_M17_MSG_TX_BUSY);
	packet_tick(RADIO_TICK_MS);
	packet_result(RADIO_M17_MSG_TX_BUSY);

	air_fit4800 = AIR_FIT_TX;
	packet_setup();
	packet_submit(m17_destination("INFO"), sm.view.m17_selected.generation);
	packet_ready();
	packet_tick(0);
	CHECK(!hw.keyed);
	packet_tick(5000);
	packet_result(RADIO_M17_MSG_TX_BUSY);
	air_fit4800 = AIR_FIT_RX | AIR_FIT_TX | AIR_FIT_SOFT;
}

static void test_packet_context_and_guards(void)
{
	packet_setup();
	uint32_t generation = sm.view.m17_selected.generation;
	packet_submit(m17_destination("INFO"), generation + 1);
	packet_result(RADIO_M17_MSG_TX_CONTEXT);
	packet_submit(0, generation);
	packet_result(RADIO_M17_MSG_TX_ADDRESS);

	packet_submit(m17_destination("INFO"), generation);
	packet_ready();
	sm.view.ch.tx_hz += 12500;	// stored selection generation alone does not describe the route
	packet_tick(0);
	CHECK(sm.view.m17_selected.generation == generation);
	packet_result(RADIO_M17_MSG_TX_CONTEXT);
	sm.view.ch.tx_hz -= 12500;

	packet_submit(m17_destination("INFO"), generation);
	packet_ready();
	sm.set.tx_inhibit = true;
	packet_tick(0);
	packet_result(RADIO_M17_MSG_TX_GUARD);
	sm.set.tx_inhibit = false;

	packet_submit(m17_destination("INFO"), generation);
	req(RREQ_M17_BERT, 1);
	packet_result(RADIO_M17_MSG_TX_GUARD);
	CHECK(radio_m17_bert_on());
	req(RREQ_M17_BERT, 3);

	packet_submit(m17_destination("INFO"), generation);
	req(RREQ_PTT, 1);
	packet_result(RADIO_M17_MSG_TX_GUARD);
	req(RREQ_PTT, 0);
	run(500);
	CHECK(!hw.keyed);

	packet_submit(m17_destination("INFO"), generation);
	req(RREQ_ZONE_POS, 0);
	packet_result(RADIO_M17_MSG_TX_CONTEXT);
}

static void test_packet_completion(void)
{
	packet_setup();
	m17_talk_to(sm.view.m17_selected.generation, M17_ADDR_BROADCAST);
	packet_start();
	const m17_engine_t *engine = radio_m17_engine_mem();
	uint64_t dst = 0;
	for (unsigned i = 0; i < M17_ADDR_BYTES; i++)
		dst = (dst << 8) | engine->lsf[i];
	CHECK(dst == m17_destination("INFO"));
	req(RREQ_PTT, 1);
	req(RREQ_PTT, 0);
	req(RREQ_M17_BERT, 2);
	CHECK(hw.keyed && fair[0].on && completions == 0 && !radio_m17_bert_on());
	packet_provider_done(AIR_TX_END_DRAINED, false);
	packet_result(RADIO_M17_MSG_TX_SENT);
	CHECK(sm.view.state == RS_IDLE && strcmp(sm.view.m17.tx_dst, "ALL") == 0);

	packet_setup();
	packet_start();
	packet_provider_done(AIR_TX_END_DRAINED, true);
	packet_result(RADIO_M17_MSG_TX_FAILED);

	packet_setup();
	packet_start();
	packet_provider_done(AIR_TX_END_ABORTED, false);
	packet_result(RADIO_M17_MSG_TX_FAILED);

	packet_setup();
	packet_start();
	packet_provider_done(AIR_TX_END_STALLED, false);
	packet_result(RADIO_M17_MSG_TX_FAILED);

	packet_setup();
	packet_start();
	packet_tick(3000);		// no provider event at all
	packet_result(RADIO_M17_MSG_TX_FAILED);

	packet_setup();
	packet_start();
	sm.view.tot_s = 1;
	packet_tick(1000);
	packet_result(RADIO_M17_MSG_TX_TIMEOUT);
	CHECK(sm.view.state == RS_IDLE && radio_m17_running());

	packet_setup();
	packet_submit(m17_destination("INFO"), sm.view.m17_selected.generation);
	packet_ready();
	tx_fail = true;
	packet_tick(5);
	tx_fail = false;
	packet_result(RADIO_M17_MSG_TX_REFUSED);
	CHECK(sm.view.state == RS_IDLE && radio_m17_running());
}

int main(void)
{
	test_packet_carrier();
	test_packet_context_and_guards();
	test_packet_completion();
	printf("# m17MessageRadioTest: %d checks, %s\n", checks, failures ? "FAILED" : "PASSED");
	return failures != 0;
}
