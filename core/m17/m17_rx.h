// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// One M17 call being received (docs/spec/m17.md 5.2, 5.5): the LSF, the six LICH chunks it is rebuilt from, and
// whether the call is still on. The framer's ctx for m17_air_mode. No RTOS call.
#ifndef RFOUNDRY_M17_RX_H
#define RFOUNDRY_M17_RX_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#include "../air/air_framer.h"
#include "m17.h"

// air_profile_t.sync[].id, most useful first (spec 1.1)
enum {
	M17_SYNC_ID_STREAM = 0,
	M17_SYNC_ID_LSF = 1,
	M17_SYNC_ID_PACKET = 2,
	M17_SYNC_ID_BERT_PRE = 3,
	M17_SYNC_ID_BERT = 4,
	M17_SYNC_ID_EOT = 5,
};

typedef struct {
	uint8_t lsf[M17_LSF_BYTES];	// bytes filled where have[] says, or the whole frame after an LSF
	bool have[6];			// LICH chunk n (LSF bytes 5n..5n+4) is in lsf; never from the frame number
	bool late;			// chunk 2 has been seen: LSF bytes 10-14 are in
	bool in_call;			// a frame this mode accepted is the call in hand
	bool crc_ok;			// lsf[0..29], CRC included, sums to 0
	bool type_known;		// lsf bytes 12-13 are this call's TYPE
	uint8_t bert_run;		// 8-symbol BERT syncs in a row presented to the head hook
#if CONFIG_M17_PACKET
	// A packet sync is not a frame by itself. The candidate bytes wait for the next packet sync one frame
	// later, and the CRC over all of them (spec 5.2).
	bool pkt_pend;
	uint32_t pkt_t;
	uint16_t pkt_len;
	uint8_t pkt_next;
	uint8_t pkt[825];
#endif
	m17_fec_work_t work;
	void (*end)(void *ctx);		// the call ended (a new LSF, a different TYPE, the end marker, the signal)
	void *ctx;
} m17_rx_t;

void m17_rx_init(m17_rx_t *rx, void (*end)(void *ctx), void *ctx);
// A retune: drop the call and the chunks. Does not call end.
void m17_rx_reset(m17_rx_t *rx);

void m17_rx_on_lsf(m17_rx_t *rx, const uint8_t lsf[M17_LSF_BYTES]);
void m17_rx_on_lich(m17_rx_t *rx, uint8_t counter, const uint8_t chunk[5]);
void m17_rx_on_lock(m17_rx_t *rx);
void m17_rx_on_lost(m17_rx_t *rx);
void m17_rx_on_eot(m17_rx_t *rx);

extern const air_mode_t m17_air_mode;

#endif
