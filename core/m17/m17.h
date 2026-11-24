// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional operation (docs/spec/m17.md): frame sizes, sync patterns, FEC constants, and addresses. The frame
// codec lives in m17_frame.c; nothing here pulls in a board or platform header.
#ifndef RFOUNDRY_M17_H
#define RFOUNDRY_M17_H

#include <stdbool.h>
#include <stdint.h>

#include "../fec/conv.h"

// ── On air (spec 1, 2) ──
#define M17_FRAME_SYMS		192		// 8 sync + 368 payload bits
#define M17_SYNC_SYMS		8
#define M17_PAYLOAD_BITS	368
#define M17_PAYLOAD_BYTES	46		// 368 bits, packed for the randomizer
#define M17_FRAME_MS		40

// 8-symbol sync words (four dibits each, MSB first in hal_air packing)
#define M17_SYNC_STREAM_BYTES	2		// FF 5D
#define M17_SYNC_PACKET_BYTES	2		// 75 FF
#define M17_SYNC_BERT_BYTES	2		// DF 55
#define M17_SYNC_END_BYTES	2		// 55 5D
#define M17_SYNC_LSF_BYTES	6		// 77 77 77 77 55 F7 (preamble tail + LSF)
#define M17_SYNC_BERT_PRE_BYTES	6		// DD DD DD DD DF 55 (preamble tail + BERT)

#define M17_PREAMBLE_SYMS	192		// 40 ms of alternating outer symbols

// ── CRC (spec 2.6) ──
#define M17_CRC_POLY		0x5935u
#define M17_CRC_INIT		0xFFFFu

// ── Convolutional code (spec app. C): rate 1/2, K=5, four zero tail bits inside the bit count ──
#define M17_CONV_CODE		((fec_conv_code_t){ .g1 = 0x19, .g2 = 0x17 })
#define M17_CONV_TAIL_BITS	4

// ── Puncturing (spec app. E): a 1 keeps the bit, 0 drops it (decoder gets soft 0) ──
#define M17_PUNCTURE_P1_LEN	61
#define M17_PUNCTURE_P1_KEEP	46
#define M17_PUNCTURE_P2_LEN	12
#define M17_PUNCTURE_P2_KEEP	11
#define M17_PUNCTURE_P3_LEN	8
#define M17_PUNCTURE_P3_KEEP	7

// Coded-bit counts before puncturing
#define M17_LSF_CODED_BITS	488		// 240 payload + 4 tail, rate 1/2
#define M17_STREAM_CODED_BITS	296		// 144 + 4 tail
#define M17_PACKET_CODED_BITS	420		// 206 + 4 tail
#define M17_BERT_CODED_BITS	402		// 197 + 4 tail; P2 leaves 369, last dropped → 368

// ── LSF (spec 3.1) ──
#define M17_LSF_BYTES		30
#define M17_LSF_CRC_BYTES	2
#define M17_LSF_DATA_BYTES	28		// bytes 0-27 covered by the CRC

// ── Stream (spec 3.2) ──
#define M17_LICH_BITS		48
#define M17_LICH_GOLAY_WORDS	4
#define M17_LICH_CODED_BITS	96
#define M17_STREAM_PAYLOAD_BITS	144	// frame number + 128 payload
#define M17_STREAM_VOICE_BITS	128
#define M17_LICH_COUNT_MAX	5

// ── Packet (spec 3.3) ──
#define M17_PACKET_DATA_BITS	206

// ── BERT (spec 3.7) ──
#define M17_BERT_BITS		197

// ── Addresses (spec 7, app. A): base 40, big endian, six bytes ──
#define M17_ADDR_BYTES		6
#define M17_ADDR_CHARS_MAX	9
#define M17_ADDR_BASE		40
#define M17_ADDR_BROADCAST	0xFFFFFFFFFFFFull

#endif