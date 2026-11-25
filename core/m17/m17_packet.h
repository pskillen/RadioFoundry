// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// A packet's bytes across its frames, and the gather that checks the CRC (docs/spec/m17.md 3.3, 3.4).
// The frame codec packs each frame; this file only splits and joins the bytes.
#ifndef RFOUNDRY_M17_PACKET_H
#define RFOUNDRY_M17_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"

#if CONFIG_M17 && CONFIG_M17_PACKET

#define M17_PACKET_MAX_FRAMES	33
#define M17_PACKET_FRAME_BYTES	25
#define M17_PACKET_MAX_BYTES	(M17_PACKET_MAX_FRAMES * M17_PACKET_FRAME_BYTES)

// Application bytes, plus the type byte and the two CRC bytes, in 1..33 frames. False outside that.
bool m17_packet_fits(uint16_t data_len);

// type, data, then the LSF CRC over both. False when it does not fit; *out_len is the byte count.
bool m17_packet_build(uint8_t type, const uint8_t *data, uint16_t data_len, uint8_t *out, uint16_t *out_len);

// Frame `index` of a built packet. On the last frame `valid` is the byte count and `frame_num` is 0.
bool m17_packet_slice(const uint8_t *pkt, uint16_t len, uint8_t index, uint8_t data[M17_PACKET_FRAME_BYTES],
		      bool *last, uint8_t *valid, uint8_t *frame_num);

typedef struct {
	uint8_t buf[M17_PACKET_MAX_BYTES];
	uint16_t len;
	uint8_t next;		// the next frame number
	bool started;
} m17_packet_gather_t;

void m17_packet_gather_init(m17_packet_gather_t *g);
// 0: more frames. 1: last frame and the CRC matches. -1: a break in the numbers, or the CRC does not.
int m17_packet_gather_frame(m17_packet_gather_t *g, const uint8_t data[M17_PACKET_FRAME_BYTES], bool last,
			    uint8_t valid, uint8_t frame_num);

#endif
#endif
