// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional channels and the radio-wide callsign (docs/spec/m17-codeplug.md)
#ifndef RFOUNDRY_M17_CODEPLUG_H
#define RFOUNDRY_M17_CODEPLUG_H

#include "codeplug/codeplug.h"

#define M17_CP_CHAN_LEN		7u
#define M17_CP_CAN_MAX		15u
#define M17_CP_SQ_OFF		0x10u
#define M17_CP_CALL_BYTES	10u
#define M17_CP_CALL_LEN		10u
#define M17_CP_ADDR_RESERVED	0xEE6B28000000ull

typedef struct {
	uint8_t can;
	bool can_squelch;
	uint64_t dst;
} m17_chan_t;

// Decode a block, using defaults for missing fields (len -1 means no block).
void m17_chan_decode(const uint8_t *b, int len, m17_chan_t *out);
// Encode a channel block, storing broadcast as zero.
void m17_chan_encode(const m17_chan_t *c, uint8_t out[M17_CP_CHAN_LEN]);
// Parse a destination, returning false for invalid text.
bool m17_dst_parse(const char *text, uint64_t *dst);
// Read the callsign record alone, leaving out empty when it is absent or invalid.
bool m17_call_decode(const uint8_t *b, int len, char out[M17_CP_CALL_LEN]);
// Pick the record or the first word of radio_name (which may be NULL), else return false with out empty.
bool m17_callsign_pick(const uint8_t *b, int len, const char *radio_name, char out[M17_CP_CALL_LEN]);
// Load the parameters of a live M17 tag on channel number.
bool m17_chan_load(uint16_t number, const channel_t *ch, m17_chan_t *out);
// Read the stored record and pick a callsign with radio_name as the fallback.
bool m17_callsign_load(const char *radio_name, char out[M17_CP_CALL_LEN]);

#endif
