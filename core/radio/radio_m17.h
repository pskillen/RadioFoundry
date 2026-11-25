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

// The radio view's part (radio_view_t.m17): the call heard now, or the last one
typedef struct {
	uint8_t flags;		// RADIO_M17_*
	uint8_t tx;		// air_tx_state_t of the session
	uint8_t can;		// the CAN in force (the channel's, not the one heard)
	uint16_t fit;		// what the board's provider gives the profile (AIR_FIT_*), 0: nothing
	char caller[RADIO_M17_CALL_LEN];	// empty until the link setup frame's CRC is 0
	char dest[RADIO_M17_CALL_LEN];
} radio_m17_view_t;

// Counters for the debug link (any task reads them; the radio task writes them while M17 runs)
typedef struct {
	uint32_t begins, begin_fails;	// engine starts, and the ones refused (no provider, no fit, no memory)
	uint32_t rx_chunks, tx_underruns, tx_syms;
	uint32_t bert_bits, bert_errors, bert_frames, bert_lost;
} radio_m17_stats_t;

#if CONFIG_M17
#include "radio/radio_air.h"

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

// Radio task. begin: the board tuned in its digital mode, its DMR stack stopped. number is the stored channel
// (0: the VFO). A blank callsign still starts (a BERT needs none). False: no memory, no provider or no fit
bool radio_m17_begin(const channel_t *ch, uint16_t number);
void radio_m17_end(void);
bool radio_m17_running(void);
void radio_m17_retuned(void);
// The settings changed: the engine takes them (not while transmitting)
void radio_m17_apply(const channel_t *ch, uint16_t number);
// Every tick: the view and the counters. True when the view changed
bool radio_m17_poll(radio_m17_view_t *v);

void radio_m17_stats(radio_m17_stats_t *out);
void radio_m17_reset_stats(void);

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
