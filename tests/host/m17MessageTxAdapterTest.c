// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 text through the radio adapter, engine, and framer to a fake provider.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_session.h"
#include "air/air_syms.h"
#include "board.h"
#include "m17/m17_codeplug.h"
#include "m17/m17_engine.h"
#include "m17/m17_rx.h"
#include "radio/radio_m17.h"

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

#define CHUNK_SYMS 144
#define DEPTH 3
#define FRAME_BYTES (M17_FRAME_SYMS / 4)

static uint64_t lend[4096 / sizeof(uint64_t)];
static m17_fec_work_t work;
static m17_packet_gather_t gathered;
static m17_engine_t incoming;
static air_tx_chunk_t queue[DEPTH];
static uint8_t wire[(M17_PACKET_MAX_FRAMES + 3) * FRAME_BYTES];
static uint8_t text[RADIO_M17_MSG_TEXT_MAX + 1];
static unsigned head, count, nsyms, begins, aborts;
static bool keyed, transmitting, refuse_go, refuse_prime;
static uint32_t fit_flags = AIR_FIT_RX | AIR_FIT_TX;
static air_event_cb_t event_cb;
static void *event_ctx;

static void event(air_event_kind_t kind, uint32_t arg)
{
	const air_event_t ev = { .kind = kind, .arg = arg };

	CHECK(event_cb != NULL);
	event_cb(&ev, event_ctx);
}

static uint32_t fit(const air_profile_t *profile)
{
	return profile == &m17_air_mode.profile ? fit_flags : 0;
}

static bool open_air(const air_profile_t *profile, air_event_cb_t cb, void *ctx)
{
	CHECK(fit(profile) && event_cb == NULL && !transmitting && count == 0);
	event_cb = cb;
	event_ctx = ctx;
	return true;
}

static void close_air(void)
{
	CHECK(!transmitting && count == 0);
	event_cb = NULL;
	event_ctx = NULL;
}

static void restart_air(void) {}

static uint8_t tx_space(void)
{
	return (uint8_t)(DEPTH - count);
}

static bool tx_queue(const air_tx_chunk_t *chunk)
{
	if (refuse_prime)
		return false;
	CHECK(count < DEPTH);
	queue[(head + count) % DEPTH] = *chunk;
	count++;
	return true;
}

static void tx_end(void)
{
	if (refuse_prime)
		return;
	CHECK(count > 0);
	queue[(head + count - 1) % DEPTH].flags |= AIR_TX_LAST;
}

static bool tx_begin(void)
{
	CHECK(keyed && !transmitting && count > 0);
	begins++;
	if (refuse_go)
		return false;
	transmitting = true;
	event(AIR_EV_TX_STARTED, 0);
	return true;
}

static void tx_abort(void)
{
	aborts++;
	count = 0;
	if (transmitting) {
		transmitting = false;
		event(AIR_EV_TX_SPACE, 0); // A deferred refill event can precede DONE.
		CHECK(count == 0);
		event(AIR_EV_TX_DONE, AIR_TX_END_ABORTED);
	}
	// Like the HRC provider, flushing before go does not emit TX_DONE.
}

static const hal_air_t air = {
	.fit = fit, .open = open_air, .close = close_air, .rx_restart = restart_air,
	.tx_chunk_syms = CHUNK_SYMS, .tx_queue_depth = DEPTH,
	.tx_begin = tx_begin, .tx_queue = tx_queue, .tx_space = tx_space, .tx_end = tx_end, .tx_abort = tx_abort,
};
const board_t board = { .name = "m17-packet-tx", .air = &air };

bool m17_chan_load(uint16_t number, const channel_t *ch, m17_chan_t *out)
{
	(void)number;
	(void)ch;
	(void)out;
	return false;
}

bool m17_callsign_load(const char *name, char out[M17_CP_CALL_LEN])
{
	strcpy(out, name);
	return name[0] != '\0';
}

void *radio_p25_idle_mem(size_t *len)
{
	*len = sizeof lend;
	return lend;
}

static const radio_m17_settings_t base = { .can = 5, .dst = M17_ADDR_BROADCAST, .callsign = "AB1CD" };
#define PRIVATE_DST 0x0000009FE391ull // AB2CD

static uint64_t addr(const uint8_t *p)
{
	uint64_t v = 0;

	for (unsigned i = 0; i < M17_ADDR_BYTES; i++)
		v = (v << 8) | p[i];
	return v;
}

static const m17_engine_t *engine(void)
{
	return radio_m17_engine_mem();
}

static void restored(void)
{
	CHECK(engine()->cfg.tx_kind == M17_TX_VOICE && engine()->cfg.dst == base.dst);
	CHECK(!keyed && !transmitting && count == 0 && radio_m17_air.tx_state() == AIR_TX_IDLE);
}

static void arm(uint64_t dst, uint16_t len)
{
	unsigned n = begins;

	CHECK(radio_m17_air.tx_state() == AIR_TX_IDLE);
	CHECK(radio_m17_packet_tx_prepare(dst, text, len));
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_PENDING);
	CHECK(!keyed && !transmitting && count == 0 && begins == n);
	CHECK(!radio_m17_packet_tx_prepare(dst, text, len));
	CHECK(!radio_m17_apply(&base, dst));
}

static void prime(void)
{
	unsigned n = begins;

	CHECK(radio_m17_air.tx_prepare());
	CHECK(!keyed && !transmitting && count > 0 && begins == n);
	nsyms = 0;
}

static void prepare(uint64_t dst, uint16_t len)
{
	arm(dst, len);
	prime();
}

static bool next_chunk(void)
{
	CHECK(transmitting && keyed && count > 0);
	const air_tx_chunk_t *chunk = &queue[head];
	bool last = (chunk->flags & AIR_TX_LAST) != 0;

	CHECK(nsyms + chunk->nsyms <= sizeof wire * 4);
	for (uint16_t i = 0; i < chunk->nsyms; i++)
		air_sym_put(wire, (uint16_t)(nsyms + i), air_sym_get(chunk->syms, i));
	nsyms += chunk->nsyms;
	head = (head + 1) % DEPTH;
	count--;
	if (last) {
		CHECK(count == 0);
		transmitting = false;
		event(AIR_EV_TX_DONE, AIR_TX_END_DRAINED);
	} else {
		event(AIR_EV_TX_SPACE, 0);
	}
	return last;
}

static void drain(void)
{
	for (unsigned i = 0; i < sizeof wire * 4 / CHUNK_SYMS + 1; i++)
		if (next_chunk())
			return;
	CHECK(false);
}

static void verify_wire(uint64_t dst, uint16_t len, uint8_t first)
{
	unsigned packet_bytes = len + 3;
	unsigned frames = (packet_bytes + M17_PACKET_FRAME_BYTES - 1) / M17_PACKET_FRAME_BYTES;
	uint8_t lsf[M17_LSF_BYTES];

	CHECK(nsyms == (frames + 3) * M17_FRAME_SYMS);
	for (unsigned i = 0; i < FRAME_BYTES; i++)
		CHECK(wire[i] == 0x77);
	const uint8_t *f = wire + FRAME_BYTES;
	CHECK(f[0] == 0x55 && f[1] == 0xF7);
	CHECK(m17_lsf_decode(f + 2, &work, lsf) == 0);
	CHECK(m17_crc16(lsf, sizeof lsf) == 0);
	CHECK(addr(lsf) == dst && addr(lsf + M17_ADDR_BYTES) == M17_ADDR_AB1CD);
	uint16_t type = (uint16_t)((lsf[M17_LSF_TYPE_OFFSET] << 8) | lsf[M17_LSF_TYPE_OFFSET + 1]);
	CHECK(type == (base.can << M17_TYPE_CAN_SHIFT));

	m17_packet_gather_init(&gathered);
	for (unsigned i = 0; i < frames; i++) {
		bool last;
		uint8_t data[M17_PACKET_FRAME_BYTES], valid, number;

		f += FRAME_BYTES;
		CHECK(f[0] == 0x75 && f[1] == 0xFF);
		CHECK(m17_packet_decode(f + 2, &work, data, &number, &last, &valid) == 0);
		CHECK(last == (i + 1 == frames));
		CHECK(last ? valid == packet_bytes - i * M17_PACKET_FRAME_BYTES : number == i);
		CHECK(m17_packet_gather_frame(&gathered, data, last, valid, number) == (last ? 1 : 0));
	}
	CHECK(gathered.len == packet_bytes && gathered.buf[0] == 0x05);
	CHECK(gathered.buf[1] == first && gathered.buf[len] == 0);
	CHECK(memcmp(gathered.buf + 2, text + 1, len - 2) == 0);
	f += FRAME_BYTES;
	for (unsigned i = 0; i < FRAME_BYTES; i += 2)
		CHECK(f[i] == 0x55 && f[i + 1] == 0x5D);
}

static void send_and_check(uint64_t dst, uint16_t len)
{
	uint8_t first = text[0];

	arm(dst, len);
	text[0] ^= 0x01;
	prime();
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_PENDING);
	radio_m17_air.tx_stop(); // A PTT release cannot truncate this packet.
	drain();
	verify_wire(dst, len, first);
	CHECK(keyed && radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_DRAINED);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_DRAINED);
}

static void refused_or_cancelled(void)
{
	unsigned n = begins;

	CHECK(radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	radio_m17_air.tx_abort(); // Before prime, e.g. a lost route or TX guard.
	restored();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);

	CHECK(radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	radio_m17_air.tx_clear();
	restored();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);

	CHECK(radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	radio_m17_retuned();
	restored();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);

	CHECK(radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	refuse_prime = true;
	CHECK(!radio_m17_air.tx_prepare());
	refuse_prime = false;
	CHECK(radio_m17_air.tx_state() == AIR_TX_DONE && radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	radio_m17_air.tx_clear();
	restored();

	prepare(PRIVATE_DST, sizeof text);
	radio_m17_air.tx_abort(); // Board key refused after prime.
	CHECK(radio_m17_air.tx_state() == AIR_TX_DONE && radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	radio_m17_air.tx_clear();
	restored();
	CHECK(begins == n);

	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	refuse_go = true;
	CHECK(!radio_m17_air.tx_go());
	refuse_go = false;
	CHECK(!transmitting && count == 0 && radio_m17_air.tx_state() == AIR_TX_DONE);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED && begins == n + 1);
}

static void failed_on_air(void)
{
	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	event(AIR_EV_TX_UNDERRUN, 0);
	drain();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();

	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	// Abort with last chunk queued: finish and DONE are both true here.
	while (radio_m17_air.tx_state() != AIR_TX_ENDING)
		CHECK(!next_chunk());
	radio_m17_air.tx_abort();
	CHECK(radio_m17_air.tx_state() == AIR_TX_DONE && radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();

	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	count = 0;
	transmitting = false;
	event(AIR_EV_TX_DONE, AIR_TX_END_DRAINED); // A premature completion cannot count as sent.
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();

	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	while (radio_m17_air.tx_state() != AIR_TX_ENDING)
		CHECK(!next_chunk());
	count = 0;
	transmitting = false;
	event(AIR_EV_TX_DONE, AIR_TX_END_STALLED);
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	keyed = false;
	radio_m17_air.tx_clear();
	restored();

	prepare(PRIVATE_DST, sizeof text);
	keyed = true;
	CHECK(radio_m17_air.tx_go());
	radio_m17_end();
	CHECK(!radio_m17_running() && radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_FAILED);
	keyed = false;
	CHECK(radio_m17_begin(&base, 0));
	restored();
}

static void lock_and_routes(void)
{
	CHECK(radio_m17_packet_tx_source() == M17_ADDR_AB1CD && !radio_m17_rx_locked());
	const m17_engine_cfg_t cfg = { .own = PRIVATE_DST, .dst = base.dst, .can = base.can, .tx_kind = M17_TX_VOICE };

	m17_engine_init(&incoming, &cfg, NULL, NULL);
	CHECK(m17_engine_tx_start(&incoming, 0, NULL, 0));
	for (unsigned i = 0; i < 3; i++) {
		uint8_t hard[FRAME_BYTES];

		CHECK(m17_engine_tx_read(&incoming, hard, sizeof hard) == sizeof hard);
		const air_chunk_t chunk = {
			.t_us = 1000000 + i * 40000, .nsyms = M17_FRAME_SYMS, .sync_at = -1, .hard = hard,
		};
		const air_event_t ev = { .kind = AIR_EV_RX, .chunk = &chunk };

		event_cb(&ev, event_ctx);
	}
	CHECK(radio_m17_rx_locked());
	radio_m17_retuned();
	CHECK(!radio_m17_rx_locked());

	CHECK(!radio_m17_packet_tx_prepare(0, text, sizeof text));
	CHECK(!radio_m17_packet_tx_prepare(M17_CP_ADDR_RESERVED, text, sizeof text));
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text + 1));
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text - 1));
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, (const uint8_t *)"x\0y", 4));

	radio_m17_settings_t broken = base;
	strcpy(broken.callsign, "   ");
	CHECK(radio_m17_apply(&broken, 0));
	CHECK(radio_m17_packet_tx_source() == 0);
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	strcpy(broken.callsign, "@AB1CD");
	CHECK(radio_m17_apply(&broken, 0));
	CHECK(radio_m17_packet_tx_source() == 0);
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	strcpy(broken.callsign, "");
	radio_m17_set_radio_name("AB1CD");
	CHECK(radio_m17_apply(&broken, 0));
	CHECK(radio_m17_packet_tx_source() == M17_ADDR_AB1CD);

	radio_m17_bert(2);
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	CHECK(engine()->cfg.tx_kind == M17_TX_BERT);
	radio_m17_bert(3);
}

int main(void)
{
	radio_m17_init();
	CHECK(radio_m17_packet_tx_status() == RADIO_M17_PACKET_TX_NONE);
	CHECK(radio_m17_begin(&base, 0));
	memcpy(text, "Caf\xC3\xA9", 6);
	send_and_check(PRIVATE_DST, 6);

	memset(text, 'x', sizeof text - 1);
	text[sizeof text - 1] = 0;
	lock_and_routes();
	send_and_check(M17_ADDR_BROADCAST, sizeof text);
	refused_or_cancelled();
	failed_on_air();

	radio_m17_end();
	fit_flags = AIR_FIT_RX;
	CHECK(radio_m17_begin(&base, 0));
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	radio_m17_end();
	CHECK(!radio_m17_rx_locked() && radio_m17_packet_tx_source() == 0);
	CHECK(!radio_m17_packet_tx_prepare(PRIVATE_DST, text, sizeof text));
	CHECK(aborts == 5);
	printf("m17MessageTxAdapterTest passed, engine %zu bytes\n", radio_m17_engine_bytes());
	return 0;
}
