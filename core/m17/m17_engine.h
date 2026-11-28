// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One M17 transmission (docs/spec/m17.md 4, 6.2): preamble, LSF, frames, end marker. Voice bytes are whatever
// tx_voice hands over. The air session that queues these bytes is the radio core's; this step builds them.
#ifndef RFOUNDRY_M17_ENGINE_H
#define RFOUNDRY_M17_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#if CONFIG_M17

#include "m17.h"
#include "m17_bert.h"

struct m17_rx;

enum {
	M17_TX_VOICE = 0,
	M17_TX_PACKET = 1,
	M17_TX_BERT = 2,
};

typedef struct {
	// in_call became true. rx is the call in hand; its LSF may still be incomplete.
	void (*rx_start)(const struct m17_rx *rx, void *ctx);
	// The LICH rebuilt an LSF whose CRC is 0. Not called for an LSF frame, which arrives whole.
	void (*rx_update)(const struct m17_rx *rx, void *ctx);
	// One stream frame that passed CAN squelch. frame_num includes bit 15.
	void (*rx_voice)(const uint8_t voice[16], uint16_t frame_num, void *ctx);
	void (*rx_lost)(void *ctx);	// a stream frame placed by counting
	void (*rx_end)(void *ctx);
	// The packet, type byte first, CRC removed. Only when CRC and CAN squelch pass.
	void (*rx_packet)(const uint8_t *data, uint16_t len, void *ctx);
	// Two codec frames for the next stream frame. False: none ready, so silence goes out.
	bool (*tx_voice)(uint8_t c2[2][8], void *ctx);
	// One stream frame after CAN squelch, including a failed m17_stream_decode. errs is that
	// return. voice and frame_num are valid only when errs >= 0; a failure passes NULL and 0.
	// Dispatched on its own. NULL keeps the legacy rx_voice callback, success only.
	void (*rx_voice_fec)(const uint8_t *voice, uint16_t frame_num, int errs, void *ctx);
} m17_engine_hooks_t;

typedef struct {
	uint64_t own;
	uint64_t dst;
	uint8_t can;
	uint8_t rx_can;
	bool can_squelch;
	uint8_t tx_kind;		// M17_TX_VOICE, M17_TX_PACKET, M17_TX_BERT
	bool tx_inhibit;
	const uint8_t *silence;		// one codec frame, 8 bytes; NULL sends zeros
} m17_engine_cfg_t;

typedef enum {
	M17_CSMA_WAIT = 0,
	M17_CSMA_SEND,
	M17_CSMA_BUSY,
} m17_csma_result_t;

// Packets only. Locked, or a chunk that was not marked no-signal, waits. After one clear frame, each 40 ms
// slot takes the byte the caller drew: 1..64 send (64 of 256, probability 1/4) and 0 does not. Five seconds
// without a send is busy. now_ms is the caller's clock.
typedef struct {
	uint32_t t0;
	uint32_t clear_since;
	uint32_t slot;
	uint8_t decided;		// 0 until SEND or BUSY
} m17_csma_t;

typedef struct {
	m17_engine_cfg_t cfg;
	m17_engine_hooks_t hooks;
	void *ctx;
	bool active;
	bool stop;
	uint8_t phase;
	uint8_t chunk[M17_FRAME_SYMS / 4];
	uint16_t len;
	uint16_t pos;
	uint8_t lsf[M17_LSF_BYTES];
	uint16_t fn;
	uint8_t lich;
	m17_prbs9_t prbs;
#if CONFIG_M17_PACKET
	uint8_t pkt[825];
	uint16_t pkt_len;
	uint8_t pkt_i;
	uint8_t pkt_n;
#endif
} m17_engine_t;

void m17_engine_init(m17_engine_t *e, const m17_engine_cfg_t *cfg, const m17_engine_hooks_t *hooks, void *ctx);
// Voice or BERT, from cfg.tx_kind: type, data and len are the packet. False when transmit is inhibited,
// or the packet would not be 1 to 33 frames.
bool m17_engine_tx_start(m17_engine_t *e, uint8_t type, const uint8_t *data, uint16_t len);
void m17_engine_tx_stop(m17_engine_t *e);
// Packed dibits, 4 symbols a byte, MSB first. 0 when the end marker has been read.
uint16_t m17_engine_tx_read(m17_engine_t *e, uint8_t *dst, uint16_t max);

void m17_csma_init(m17_csma_t *c);
m17_csma_result_t m17_csma_poll(m17_csma_t *c, uint32_t now_ms, bool locked, bool signal, uint8_t coin);

#endif
#endif
