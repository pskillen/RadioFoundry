// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One M17 call being received (docs/spec/m17.md 5.2, 5.5): the LSF, the six LICH chunks it is rebuilt from, and
// whether the call is still on. The framer's ctx for m17_air_mode. No RTOS call.
#ifndef RFOUNDRY_M17_RX_H
#define RFOUNDRY_M17_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#if CONFIG_M17

#include "../air/air_framer.h"
#include "m17.h"
#include "m17_engine.h"
#if CONFIG_M17_PACKET
#include "m17_packet.h"
#endif

// air_profile_t.sync[].id, most useful first (spec 1.1)
enum {
	M17_SYNC_ID_STREAM = 0,
	M17_SYNC_ID_LSF = 1,
	M17_SYNC_ID_PACKET = 2,
	M17_SYNC_ID_BERT_PRE = 3,
	M17_SYNC_ID_BERT = 4,
	M17_SYNC_ID_EOT = 5,
};

#if CONFIG_M17_PACKET
enum {
	M17_PKT_NONE,
	M17_PKT_LSF,		// fresh, explicit packet LSF
	M17_PKT_MORE,		// expecting the next packet frame
	M17_PKT_ONE,		// expecting the EOT of a one-frame packet
};

typedef struct {
	uint32_t crc_ok;		// confirmed packets with a valid CRC, including CAN rejects
	uint32_t crc_fail;	// confirmed final frames with a bad packet CRC
	uint32_t fec_reject;	// timed packet candidates rejected by the decoder or disagreement limit
	uint32_t can_reject;	// CRC-valid packets rejected by CAN squelch
	uint32_t fec_samples;	// nonnegative decoder results, including above-limit frames
	uint16_t fec_peak;
} m17_packet_rx_stats_t;
#endif

typedef struct m17_rx {
	uint8_t lsf[M17_LSF_BYTES];	// bytes filled where have[] says, or the whole frame after an LSF
	uint8_t lich[M17_LSF_BYTES];	// candidate for the current superframe
	bool have[6];			// LICH chunk n (LSF bytes 5n..5n+4) is in lsf; never from the frame number
	uint8_t lich_next;		// next consecutive LICH counter; 0 starts a superframe
	uint8_t meta_text[M17_META_TEXT_BYTES];
	uint8_t meta_mask, meta_have;	// declared and received text blocks
	bool link;			// LSF or stream accepted, as opposed to BERT or packet lock alone
	bool late;			// chunk 2 has been seen: LSF bytes 10-14 are in
	bool in_call;			// a frame this mode accepted is the call in hand
	bool crc_ok;			// lsf[0..29], CRC included, sums to 0
	bool type_known;		// lsf bytes 12-13 are this call's TYPE
	uint8_t rx_can;
	bool can_squelch;
	uint8_t bert_run;		// 8-symbol BERT syncs in a row presented to the head hook
	// Bits bert_prbs decoded while proving the frame. Counted from here so the frame hook does not decode again.
	uint8_t bert_bits[M17_BERT_BYTES];
	bool bert_have;
	m17_bert_count_t bert;
#if CONFIG_M17_PACKET
	uint8_t pkt_phase;
	uint32_t pkt_t;		// start of the LSF or the most recent provisional packet frame
	uint16_t pkt_len;
	uint8_t pkt_next;
	uint8_t pkt[M17_PACKET_MAX_BYTES];
	m17_packet_rx_stats_t pkt_stats;
#endif
	m17_fec_work_t work;
	const m17_engine_hooks_t *hooks;	// NULL: nothing is called
	void *ctx;
} m17_rx_t;

// hooks may be NULL. The pointer is kept, not copied: it has to outlive the call.
void m17_rx_init(m17_rx_t *rx, const m17_engine_hooks_t *hooks, void *ctx);
void m17_rx_set_can(m17_rx_t *rx, uint8_t can, bool squelch);
bool m17_rx_can_passes(const m17_rx_t *rx);
uint16_t m17_rx_type(const m17_rx_t *rx);
// A retune: keep the settings and drop the call and chunks. Does not call rx_end.
void m17_rx_reset(m17_rx_t *rx);

void m17_rx_on_lsf(m17_rx_t *rx, const uint8_t lsf[M17_LSF_BYTES]);
void m17_rx_on_lich(m17_rx_t *rx, uint8_t counter, const uint8_t chunk[5]);
void m17_rx_on_gap(m17_rx_t *rx);
void m17_rx_on_lock(m17_rx_t *rx);
void m17_rx_on_lost(m17_rx_t *rx);
void m17_rx_on_eot(m17_rx_t *rx);

#if CONFIG_M17_PACKET
void m17_rx_packet_end(m17_rx_t *rx);
#endif

extern const air_mode_t m17_air_mode;

#endif
#endif
