// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional on a channel, the radio core's side (docs/spec/m17.md 10): the engine on memory lent by the
// modes that are not running, the channel's M17 parameters, and a compact view. radio_sm.c decides when M17 runs
// and keys the transmitter (radio_air.h); nothing here keys it.
//
// A stored kind-6 channel runs on its block. The VFO and a channel in M17 for now run on the RAM settings,
// with CAN squelch forced on for the latter (decision 35).
#ifndef RADIO_M17_H
#define RADIO_M17_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"
#include "codeplug/codeplug.h"
#if CONFIG_M17_PACKET
#include "radio/radio_m17_msg.h"
#endif

#define RADIO_M17_CAN_MAX	15
#define RADIO_M17_CALL_LEN	10	// nine base-40 characters and a NUL (docs/spec/m17.md 7)

// The M17 settings (RAM only, reset at start-up): the VFO's
typedef struct {
	uint8_t can;			// 0 to RADIO_M17_CAN_MAX, sent on every transmission
	bool can_squelch;		// play only this CAN; off plays every one
	uint64_t dst;			// destination address; 0 is stored as the broadcast address
	char callsign[RADIO_M17_CALL_LEN];	// session override via debug link, else the record, else the radio name's first word
} radio_m17_settings_t;

// view flags
#define RADIO_M17_ON		0x01u	// the engine runs on this channel
#define RADIO_M17_RX		0x02u	// a call is being received
#define RADIO_M17_LATE		0x04u	// ... and was entered without its link setup frame

typedef enum {
	RADIO_M17_RX_NONE,
	RADIO_M17_RX_UNKNOWN,
	RADIO_M17_RX_OTHER_CAN,
	RADIO_M17_RX_ENCRYPTED,
	RADIO_M17_RX_VOICE_DATA,
	RADIO_M17_RX_DATA,
	RADIO_M17_RX_VOICE,
} radio_m17_rx_state_t;

#define RADIO_M17_META_LEN	53	// four 13-byte META blocks and a NUL

// The radio view's part (radio_view_t.m17): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_M17_*
	uint8_t tx;		// air_tx_state_t of the session
	uint8_t can;		// the CAN in force (the channel's, not the one heard)
	uint16_t fit;		// what the board's provider gives the profile (AIR_FIT_*), 0: nothing
	char caller[RADIO_M17_CALL_LEN];	// empty until the link setup frame's CRC is 0
	char dest[RADIO_M17_CALL_LEN];	// the address heard, not the one PTT calls
	char tx_dst[RADIO_M17_CALL_LEN];	// the destination PTT calls; ALL is broadcast
#if CONFIG_M17
	uint8_t rx_state;	// radio_m17_rx_state_t
	uint8_t rx_can;		// UINT8_MAX until TYPE is known
	char via[RADIO_M17_CALL_LEN];
	char meta[RADIO_M17_META_LEN];
#endif
} radio_m17_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while M17 runs)
typedef struct {
	uint32_t begins, begin_fails;	// engine starts, and the ones refused (no provider, no fit, no memory)
	uint32_t rx_chunks, tx_underruns, tx_syms;
	uint32_t bert_bits, bert_errors, bert_frames, bert_lost;
} radio_m17_stats_t;

#if CONFIG_M17
#include "radio/radio_air.h"

// Identity of a selected M17 channel. A change to another mode makes this view unavailable.
typedef struct {
	bool is_vfo;
	uint8_t vfo_index;
	union {
		struct { uint32_t rx_hz, tx_hz; } vfo;
		struct { uint16_t zone, pos, number; } stored;
	};
} radio_m17_context_t;

typedef struct {
	uint64_t base_dst, effective_dst;
	uint32_t generation;		// zero when there is no selected M17 channel
	radio_m17_context_t context;
	uint8_t can;
} radio_m17_selected_t;

extern const radio_air_ops_t radio_m17_air;

void radio_m17_init(void);
// The board's provider gives M17 something (receive or transmit): what the UI and the mode request offer
bool radio_m17_fits(void);
// What the provider gives the M17 profile (AIR_FIT_*; any task)
uint32_t radio_m17_fit(void);
// A channel read from the codeplug, as the radio runs it: a stored M17 channel (kind 6) becomes RADIO_MODE_M17
// where the board carries M17 and its frequency allows a digital mode. False: left as read
bool radio_m17_take(channel_t *ch);
// The board's provider receives channel ch (any task): what scan and dual watch visit
bool radio_m17_receives(const channel_t *ch, uint16_t number);
void radio_m17_settings(radio_m17_settings_t *out);		// any task
void radio_m17_set_settings(const radio_m17_settings_t *s);	// radio task
// The radio name, the callsign's fallback. Radio task; taken at the next begin or apply.
void radio_m17_set_radio_name(const char *name);

// The parameters in force, as settings: false when the engine isn't running
bool radio_m17_in_force(radio_m17_settings_t *out);

// Radio task. Resolve the stored channel's block or the session settings (number 0: VFO).
void radio_m17_base(const channel_t *ch, uint16_t number, radio_m17_settings_t *out);
// The board is tuned in digital mode, its DMR stack stopped. A blank callsign still starts for BERT.
// dst is the selected channel's temporary destination, or zero to use its base.
bool radio_m17_begin(const radio_m17_settings_t *base, uint64_t dst);
void radio_m17_end(void);
bool radio_m17_running(void);
void radio_m17_retuned(void);
// The settings changed: true if an idle engine took them.
bool radio_m17_apply(const radio_m17_settings_t *base, uint64_t dst);
// Every tick: the view and the counters. True when the view changed
bool radio_m17_poll(radio_m17_view_t *v);
// Why the last prepare refused: 0, or 1 when a voice or packet transmission has no callsign. Anything else stays 0
// so the caller keeps the generic refusal. BERT does not set this.
uint8_t radio_m17_prepare_deny(void);

void radio_m17_stats(radio_m17_stats_t *out);
void radio_m17_reset_stats(void);

#if CONFIG_M17_PACKET
typedef enum {
	RADIO_M17_PACKET_TX_NONE,
	RADIO_M17_PACKET_TX_PENDING,
	RADIO_M17_PACKET_TX_DRAINED,
	RADIO_M17_PACKET_TX_FAILED,
} radio_m17_packet_tx_status_t;

// Radio task. Current framer lock and resolved nonreserved source address (zero if unavailable).
bool radio_m17_rx_locked(void);
uint64_t radio_m17_packet_tx_source(void);
// Arm one idle voice session with a resolved destination and text ending in one NUL. The engine copies
// the payload synchronously. On success use the normal air prepare/go path. Abort or clear to discard.
bool radio_m17_packet_tx_prepare(uint64_t dst, const uint8_t *data, uint16_t len);
// Last armed packet's status, retained across clear and teardown until the next arm or init.
// The radio owner must drop carrier before reporting DRAINED as sent.
radio_m17_packet_tx_status_t radio_m17_packet_tx_status(void);

typedef struct {
	radio_m17_msg_summary_t summary;
	uint32_t last_received_id;	// changes on insertion and survives deletion of the newest entry
} radio_m17_inbox_t;

typedef struct {
	uint32_t crc_ok, crc_fail, fec_reject, can_reject, fec_samples;
	uint32_t unsupported_type, bad_address, malformed_text;
	uint16_t fec_peak;
} radio_m17_packet_stats_t;

// Snapshot bytes shown as m17.packet.rx_hex (wire type and text, no final NUL).
#define RADIO_M17_PACKET_DL_HEX_MAX 48u

typedef struct {
	uint32_t rx_seq;
	char rx_src[RADIO_M17_CALL_LEN];
	uint16_t rx_len;
	bool rx_cut;
	uint8_t rx_wire_len;
	uint8_t rx_wire[RADIO_M17_PACKET_DL_HEX_MAX];
} radio_m17_packet_rx_snap_t;

typedef struct {
	uint64_t tx_seq;	// the armed request serial; correlate with admission
	uint64_t tx_dst;
	uint8_t tx_result;	// 0 while in flight, else radio_m17_msg_tx_reason_t
} radio_m17_packet_tx_snap_t;

typedef struct {
	void (*lock)(void);
	void (*unlock)(void);
	uint32_t (*now_ms)(void);
} radio_m17_inbox_ops_t;

// The task layer binds these for the life of the radio core, before init or provider open.
void radio_m17_inbox_bind(const radio_m17_inbox_ops_t *ops);
// Any task, with caller-owned outputs. Timestamps are milliseconds since boot.
void radio_m17_inbox_get(radio_m17_inbox_t *out);
bool radio_m17_inbox_copy(uint32_t id, radio_m17_msg_entry_t *out);
bool radio_m17_inbox_copy_at(size_t index, radio_m17_msg_entry_t *out);
bool radio_m17_inbox_read(uint32_t id);
bool radio_m17_inbox_delete(uint32_t id);
void radio_m17_packet_stats(radio_m17_packet_stats_t *out);
void radio_m17_packet_rx_snap(radio_m17_packet_rx_snap_t *out);
void radio_m17_packet_tx_snap(radio_m17_packet_tx_snap_t *out);
// Radio task: update the last armed or completed message send for the debug link.
void radio_m17_packet_tx_snap_arm(uint64_t serial, uint64_t dst);
void radio_m17_packet_tx_snap_note(uint64_t serial, uint8_t reason);
#endif

// Radio task. 1: count received BERT from here, 2: the next transmission is BERT, 3: stop and return to voice.
// The flag is what the debug link shows; any task may read it
void radio_m17_bert(uint8_t cmd);
bool radio_m17_bert_on(void);

// The block the engine runs in, while it runs (8-byte aligned); NULL otherwise. Its size is the lend the
// session needs
const void *radio_m17_engine_mem(void);
size_t radio_m17_engine_bytes(void);
#endif

#endif
