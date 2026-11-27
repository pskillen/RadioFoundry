// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// An M17 provider through the framer and radio inbox. Single-threaded task-gate and clock stand-ins.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_syms.h"
#include "board.h"
#include "m17/m17_codeplug.h"
#include "m17/m17_rx.h"
#include "radio/radio_m17.h"

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

#define FRAME_BYTES (M17_FRAME_SYMS / 4)
#define CHUNK_SYMS 48

static uint64_t lend[4096 / sizeof(uint64_t)];
static m17_engine_t tx;
static m17_fec_work_t work;
static uint8_t wire[(M17_PACKET_MAX_FRAMES + 3) * FRAME_BYTES];
static uint8_t text[RADIO_M17_MSG_TEXT_MAX + 1];
static air_event_cb_t event_cb;
static void *event_ctx;
static bool locked, during_open, during_restart;
static unsigned restarts;
static uint32_t air_time, ticks;

static void lock(void) { CHECK(!locked); locked = true; }
static void unlock(void) { CHECK(locked); locked = false; }
static uint32_t clock_ms(void) { CHECK(!locked); return ++ticks; }

static uint64_t addr(const char *call)
{
	uint8_t raw[M17_ADDR_BYTES];

	CHECK(m17_addr_encode(call, raw));
	uint64_t encoded = 0;
	for (unsigned i = 0; i < sizeof raw; i++)
		encoded = (encoded << 8) | raw[i];
	return encoded;
}

static unsigned packet(uint64_t dst, uint8_t can, uint8_t proto, uint16_t len)
{
	m17_engine_cfg_t cfg = { .own = M17_ADDR_AB1CD, .dst = dst, .can = can, .tx_kind = M17_TX_PACKET };

	m17_engine_init(&tx, &cfg, NULL, NULL);
	CHECK(m17_engine_tx_start(&tx, proto, text, len));
	unsigned n = m17_engine_tx_read(&tx, wire, sizeof wire);
	CHECK(n && n % FRAME_BYTES == 0);
	return n / FRAME_BYTES;
}

static void feed(const uint8_t *p, unsigned frames)
{
	CHECK(event_cb != NULL && !locked);
	for (unsigned at = 0; at < frames * M17_FRAME_SYMS; at += CHUNK_SYMS) {
		int8_t soft[CHUNK_SYMS];
		const air_chunk_t chunk = {
			.t_us = air_time, .nsyms = CHUNK_SYMS, .sync_at = -1, .hard = p + at / 4, .soft = soft,
		};
		const air_event_t event = { .kind = AIR_EV_RX, .chunk = &chunk };

		for (uint16_t i = 0; i < CHUNK_SYMS; i++)
			soft[i] = (int8_t)(air_sym_level(air_sym_get(chunk.hard, i)) * 32);
		event_cb(&event, event_ctx);
		air_time += 10000;
	}
}

static void short_message(uint64_t dst, uint8_t can, uint8_t proto)
{
	memcpy(text, "Caf\xC3\xA9", 6);
	unsigned frames = packet(dst, can, proto, 6);
	feed(wire, frames);
}

static uint32_t fit(const air_profile_t *profile)
{
	return profile == &m17_air_mode.profile ? AIR_FIT_RX | AIR_FIT_SOFT : 0;
}

static bool open_air(const air_profile_t *profile, air_event_cb_t cb, void *ctx)
{
	CHECK(!locked && fit(profile));
	event_cb = cb;
	event_ctx = ctx;
	air_time = 1000000;
	if (during_open) {
		short_message(addr("AB2CD"), 5, 5);
		during_open = false;
	}
	return true;
}

static void close_air(void)
{
	CHECK(!locked);
	event_cb = NULL;
	event_ctx = NULL;
	memset(wire, 0, sizeof wire);
}

static void restart_air(void)
{
	CHECK(!locked);
	restarts++;
	if (during_restart)
		short_message(addr("AB3CD"), 5, 5);
}

static const hal_air_t air = { .fit = fit, .open = open_air, .close = close_air, .rx_restart = restart_air };
const board_t board = { .name = "m17-inbox", .air = &air };

void *radio_p25_idle_mem(size_t *len)
{
	*len = sizeof lend;
	return lend;
}

bool m17_chan_load(uint16_t number, const channel_t *ch, m17_chan_t *out)
{
	(void)number;
	(void)ch;
	(void)out;
	return false;
}

bool m17_callsign_load(const char *name, char out[M17_CP_CALL_LEN])
{
	(void)name;
	out[0] = '\0';
	return false;
}

int main(void)
{
	static const radio_m17_inbox_ops_t ops = { lock, unlock, clock_ms };
	radio_m17_settings_t base = { .can = 5, .can_squelch = true, .callsign = "AB2CD" };
	radio_m17_inbox_t inbox;
	radio_m17_msg_entry_t entry;
	radio_m17_packet_stats_t stats;
	radio_m17_view_t view = { 0 };

	radio_m17_inbox_bind(&ops);
	radio_m17_init();
	during_open = true;
	CHECK(radio_m17_begin(&base, 0));
	short_message(M17_ADDR_BROADCAST, 5, 5);
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.count == 2 && inbox.summary.received_seq == 2 && inbox.summary.unread == 2);
	uint32_t second = inbox.last_received_id;
	CHECK(second != 0 && second == inbox.summary.newest_id);
	CHECK(radio_m17_inbox_copy_at(1, &entry) && entry.id != second && entry.timestamp == 1);
	uint32_t first = entry.id;
	uint8_t sender[M17_ADDR_BYTES];
	CHECK(m17_addr_encode("AB1CD", sender));
	CHECK(memcmp(entry.message.caller, sender, sizeof sender) == 0);
	CHECK(strcmp(entry.message.text, "Caf\xE9") == 0 && entry.message.text_octets == 5 && !entry.message.cut);

	short_message(addr("ALL"), 5, 5);
	strcpy(base.callsign, "AB3CD");
	CHECK(radio_m17_apply(&base, 0));
	short_message(addr("AB2CD"), 5, 5); // Old own call.
	short_message(addr("AB3CD"), 6, 5); // CAN mismatch.
	short_message(addr("AB3CD"), 5, 4); // Unsupported protocol.
	text[0] = 'x';
	unsigned frames = packet(addr("AB3CD"), 5, 5, 1); // Missing NUL.
	feed(wire, frames);
	frames = packet(addr("AB3CD"), 5, 5, 1);
	uint8_t lsf[M17_LSF_BYTES];
	CHECK(m17_lsf_decode(wire + FRAME_BYTES + 2, &work, lsf) >= 0);
	lsf[M17_LSF_TYPE_OFFSET + 1] |= 1u << M17_TYPE_ENC_SHIFT;
	m17_lsf_encode(lsf, wire + FRAME_BYTES + 2);
	feed(wire, frames);

	const unsigned lengths[] = { 143, 144, RADIO_M17_MSG_TEXT_MAX };
	for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; i++) {
		unsigned len = lengths[i];

		memset(text, 'a', len);
		text[len] = '\0';
		frames = packet(addr("AB3CD"), 5, 5, (uint16_t)(len + 1));
		feed(wire, frames);
		CHECK(radio_m17_inbox_copy_at(0, &entry) && entry.message.text_octets == len);
		CHECK(strlen(entry.message.text) == RADIO_M17_MSG_DISPLAY_MAX);
		CHECK(entry.message.cut == (len > RADIO_M17_MSG_DISPLAY_MAX));
	}
	CHECK(!radio_m17_inbox_copy(first, &entry) && !radio_m17_inbox_copy(second, &entry));

	// Keep the FEC valid but break the packet's CRC, then send one above the FEC cutoff.
	frames = packet(addr("AB3CD"), 5, 5, 6);
	uint8_t *payload = wire + 2 * FRAME_BYTES + 2;
	uint8_t decoded[M17_PACKET_FRAME_BYTES], fn, valid;
	bool final;
	CHECK(m17_packet_decode(payload, &work, decoded, &fn, &final, &valid) == 0 && final && valid >= 2);
	decoded[valid - 1] ^= 1;
	m17_packet_encode(decoded, fn, final, valid, payload);
	feed(wire, frames);
	frames = packet(addr("AB3CD"), 5, 5, 6);
	for (uint16_t i = 0; i < 21; i++) {
		uint16_t bit = m17_interleave_qpp((uint16_t)(4 + 17 * i));
		air_bit_put(payload, bit, (uint8_t)!air_bit_get(payload, bit));
	}
	feed(wire, frames);

	radio_m17_poll(&view);
	radio_m17_packet_stats(&stats);
	CHECK(stats.crc_ok == 11 && stats.can_reject == 1 && stats.crc_fail == 1 && stats.fec_reject == 1);
	CHECK(stats.fec_peak == 21);
	CHECK(stats.unsupported_type == 2 && stats.bad_address == 1 && stats.malformed_text == 1);

	// An in-flight one-frame packet cannot finish after retune. Its FEC sample carries forward.
	frames = packet(addr("AB3CD"), 5, 5, 6);
	CHECK(frames == 4);
	feed(wire, frames - 1);
	radio_m17_retuned();
	CHECK(restarts == 1);
	feed(wire + (frames - 1) * FRAME_BYTES, 1);
	radio_m17_end();
	memset(lend, 0xA5, sizeof lend);
	radio_m17_packet_stats_t after;
	radio_m17_packet_stats(&after);
	CHECK(after.crc_ok == stats.crc_ok && after.fec_samples == stats.fec_samples + 1);
	CHECK(after.crc_fail == 1 && after.fec_reject == 1 && after.can_reject == 1 && after.fec_peak == 21);
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.count == 4 && inbox.summary.received_seq == 6);
	uint32_t last = inbox.last_received_id;
	CHECK(radio_m17_inbox_read(last) && radio_m17_inbox_copy(last, &entry) && !entry.unread);
	CHECK(entry.message.text[0] == 'a' && entry.timestamp == 6);
	CHECK(radio_m17_inbox_delete(last) && !radio_m17_inbox_copy(last, &entry));
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.count == 3 && inbox.summary.unread == 3 && inbox.summary.received_seq == 6);
	CHECK(inbox.last_received_id == last && inbox.summary.newest_id != last);

	CHECK(radio_m17_begin(&base, 0));
	during_restart = true;
	radio_m17_retuned();
	during_restart = false;
	CHECK(restarts == 2);
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.received_seq == 6 && inbox.last_received_id == last);
	short_message(addr("AB3CD"), 5, 5);
	radio_m17_reset_stats();
	radio_m17_poll(&view);
	radio_m17_packet_stats(&stats);
	CHECK(stats.crc_ok == 0 && stats.fec_samples == 0 && stats.unsupported_type == 0);
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.count == 4 && inbox.summary.received_seq == 7);
	short_message(addr("AB3CD"), 5, 5);
	radio_m17_end();
	radio_m17_packet_stats(&stats);
	CHECK(stats.crc_ok == 1 && stats.fec_samples == 1);

	radio_m17_init();
	radio_m17_inbox_get(&inbox);
	CHECK(inbox.summary.count == 0 && inbox.summary.received_seq == 0 && inbox.last_received_id == 0);
	printf("m17MessageRxTest passed, engine %zu bytes, inbox store %zu bytes\n",
	       radio_m17_engine_bytes(), sizeof(radio_m17_msg_store_t));
	return 0;
}
