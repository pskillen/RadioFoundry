// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional operation (docs/spec/m17.md): frame sizes, sync patterns, FEC constants, and addresses. The frame
// codec lives in m17_frame.c; nothing here pulls in a board or platform header.
#ifndef RFOUNDRY_M17_H
#define RFOUNDRY_M17_H

#include <stdbool.h>
#include <stddef.h>
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
#define M17_STREAM_AIR_BITS	272		// punctured conv payload in a stream frame (96 + 272 = 368)
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

// Example from the spec (7): AB1CD
#define M17_ADDR_AB1CD		0x0000009FDD51ull

// Viterbi workspace (m17_frame.c). The caller owns it.
typedef struct {
	int8_t soft[M17_LSF_CODED_BITS * 2];
	uint16_t dec[256];
} m17_fec_work_t;

uint16_t m17_crc16(const uint8_t *data, uint16_t len);

uint16_t m17_interleave_qpp(uint16_t x);
void m17_interleave(const uint8_t *in, uint8_t *out);
void m17_deinterleave(const uint8_t *in, uint8_t *out);
uint16_t m17_puncture(const uint8_t *pat, uint8_t plen, const uint8_t *coded, uint16_t ncoded, uint8_t *out);
uint16_t m17_puncture_drop_last(const uint8_t *pat, uint8_t plen, const uint8_t *coded, uint16_t ncoded,
				uint8_t *out);

bool m17_addr_encode(const char *text, uint8_t out[M17_ADDR_BYTES]);
bool m17_addr_decode(const uint8_t in[M17_ADDR_BYTES], char *out, size_t outsz);

void m17_lsf_encode(uint8_t lsf[M17_LSF_BYTES], uint8_t payload[M17_PAYLOAD_BYTES]);
int m17_lsf_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t lsf[M17_LSF_BYTES]);

void m17_stream_encode(const uint8_t lsf[M17_LSF_BYTES], uint8_t lich_count, uint16_t frame_num,
		       const uint8_t stream_pl[M17_STREAM_VOICE_BITS / 8], uint8_t payload[M17_PAYLOAD_BYTES]);
int m17_stream_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t *lich_count,
		      uint8_t lich_chunk[5], uint16_t *frame_num, uint8_t stream_pl[M17_STREAM_VOICE_BITS / 8],
		      int *lich_errs);
// The four Golay(24,12) words of a stream frame's LICH, in air order, so a caller can bound each word's
// corrections. Derandomized and deinterleaved here; the randomizer table stays in this file.
void m17_stream_lich_words(const uint8_t payload[M17_PAYLOAD_BYTES], uint32_t words[4]);

void m17_packet_pack(const uint8_t data[25], uint8_t frame_num, bool last, uint8_t valid_bytes, uint8_t bits[26]);
void m17_packet_unpack(const uint8_t bits[26], uint8_t data[25], uint8_t *frame_num, bool *last,
		       uint8_t *valid_bytes);

void m17_packet_encode(const uint8_t data[25], uint8_t frame_num, bool last, uint8_t valid_bytes,
		       uint8_t payload[M17_PAYLOAD_BYTES]);
int m17_packet_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t data[25],
		      uint8_t *frame_num, bool *last, uint8_t *valid_bytes);

void m17_bert_encode(uint8_t payload[M17_PAYLOAD_BYTES]);
// The same coding as m17_bert_encode, for 197 bits the caller supplies (a running PRBS). The four tail bits stay 0.
void m17_bert_encode_bits(const uint8_t bits[], uint8_t payload[M17_PAYLOAD_BYTES]);
int m17_bert_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t out[M17_BERT_BITS / 8]);

#endif