// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 frame codec (docs/spec/m17.md): CRC, addresses, puncturing, interleaving, randomizing, and LSF, stream, packet
// and BERT payloads. No RTOS or board code.
#include "m17.h"

#include <string.h>

#include "../fec/golay.h"

typedef struct {
	const uint8_t *pat;
	uint8_t plen;
	uint16_t nbits;		// input bits (data, CRC, tail)
	uint16_t ncoded;	// coded bits before puncturing
	uint16_t nkeep;		// kept bits after puncturing (on air in plain368)
} m17_conv_path_t;

// ── Bit helpers (MSB first in each byte) ──
static uint8_t bit_get(const uint8_t *p, uint16_t i)
{
	return (uint8_t)((p[i >> 3] >> (7 - (i & 7))) & 1);
}

static void bit_put(uint8_t *p, uint16_t i, uint8_t b)
{
	uint8_t m = (uint8_t)(0x80u >> (i & 7));

	if (b)
		p[i >> 3] |= m;
	else
		p[i >> 3] &= (uint8_t)~m;
}

static void bits_from_bytes(const uint8_t *in, uint16_t nbits, uint8_t *bits)
{
	memset(bits, 0, (size_t)(nbits + 7) / 8);
	for (uint16_t i = 0; i < nbits; i++)
		bit_put(bits, i, bit_get(in, i));
}

static void bytes_from_bits(const uint8_t *bits, uint16_t nbits, uint8_t *out)
{
	memset(out, 0, (size_t)(nbits + 7) / 8);
	for (uint16_t i = 0; i < nbits; i++)
		bit_put(out, i, bit_get(bits, i));
}

// ── CRC-16 (spec 2.6) ──
uint16_t m17_crc16(const uint8_t *data, uint16_t len)
{
	uint16_t r = M17_CRC_INIT;

	for (uint16_t i = 0; i < len; i++) {
		for (uint8_t b = 0; b < 8; b++) {
			uint8_t fb = (uint8_t)(((r >> 15) & 1) ^ ((data[i] >> (7 - b)) & 1));

			r = (uint16_t)((r << 1) & 0xFFFFu);
			if (fb)
				r ^= M17_CRC_POLY;
		}
	}
	return r;
}

// ── Puncture patterns ──
static const uint8_t punct_p1[M17_PUNCTURE_P1_LEN] = {
	1,
	1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
	1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
	1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
	1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
};
static const uint8_t punct_p2[M17_PUNCTURE_P2_LEN] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
static const uint8_t punct_p3[M17_PUNCTURE_P3_LEN] = { 1, 1, 1, 1, 1, 1, 1, 0 };

static uint16_t puncture_bits(const m17_conv_path_t *path, const uint8_t *coded, uint8_t *out)
{
	uint16_t y = 0;

	for (uint16_t i = 0; i < path->ncoded; i++) {
		if (path->pat[i % path->plen])
			bit_put(out, y++, bit_get(coded, i));
	}
	return y;
}

static void unpuncture_soft(const m17_conv_path_t *path, const uint8_t *keep, uint16_t nkeep, int8_t *soft)
{
	uint16_t y = 0;

	(void)nkeep;

	for (uint16_t i = 0; i < path->ncoded; i++) {
		if (path->pat[i % path->plen]) {
			soft[i] = bit_get(keep, y++) ? 32 : -32;
		} else {
			soft[i] = 0;
		}
	}
}

// ── Randomizer ──
static const uint8_t randomizer[M17_PAYLOAD_BYTES] = {
	0xD6, 0xB5, 0xE2, 0x30, 0x82, 0xFF, 0x84, 0x62, 0xBA, 0x4E, 0x96, 0x90, 0xD8, 0x98, 0xDD, 0x5D,
	0x0C, 0xC8, 0x52, 0x43, 0x91, 0x1D, 0xF8, 0x6E, 0x68, 0x2F, 0x35, 0xDA, 0x14, 0xEA, 0xCD, 0x76,
	0x19, 0x8D, 0xD5, 0x80, 0xD1, 0x33, 0x87, 0x13, 0x57, 0x18, 0x2D, 0x29, 0x78, 0xC3,
};

static void xor_randomizer(uint8_t *bits)
{
	for (uint16_t i = 0; i < M17_PAYLOAD_BITS; i++)
		bit_put(bits, i, (uint8_t)(bit_get(bits, i) ^ (uint8_t)((randomizer[i >> 3] >> (7 - (i & 7))) & 1)));
}

// ── Interleaver ──
uint16_t m17_interleave_qpp(uint16_t x)
{
	return (uint16_t)((45u * x + 92u * x * x) % M17_PAYLOAD_BITS);
}

static void interleave_bits(const uint8_t *in, uint8_t *out)
{
	memset(out, 0, (M17_PAYLOAD_BITS + 7) / 8);
	for (uint16_t x = 0; x < M17_PAYLOAD_BITS; x++)
		bit_put(out, m17_interleave_qpp(x), bit_get(in, x));
}

static void deinterleave_bits(const uint8_t *in, uint8_t *out)
{
	memset(out, 0, (M17_PAYLOAD_BITS + 7) / 8);
	for (uint16_t x = 0; x < M17_PAYLOAD_BITS; x++)
		bit_put(out, x, bit_get(in, m17_interleave_qpp(x)));
}

static void pack_bits(const uint8_t *bits, uint8_t out[M17_PAYLOAD_BYTES])
{
	memcpy(out, bits, M17_PAYLOAD_BYTES);
}

static void airify(uint8_t *plain)
{
	uint8_t tmp[M17_PAYLOAD_BYTES];

	interleave_bits(plain, tmp);
	memcpy(plain, tmp, M17_PAYLOAD_BYTES);
	xor_randomizer(plain);
}

static void deairify(const uint8_t air[M17_PAYLOAD_BYTES], uint8_t plain[M17_PAYLOAD_BYTES])
{
	uint8_t tmp[M17_PAYLOAD_BYTES];

	memcpy(tmp, air, M17_PAYLOAD_BYTES);
	xor_randomizer(tmp);
	deinterleave_bits(tmp, plain);
}

// ── Addresses ──
static int addr_val(char c)
{
	if (c >= 'a' && c <= 'z')
		c = (char)(c - 'a' + 'A');
	if (c == ' ')
		return 0;
	if (c >= 'A' && c <= 'Z')
		return c - 'A' + 1;
	if (c >= '0' && c <= '9')
		return c - '0' + 27;
	if (c == '-')
		return 37;
	if (c == '/')
		return 38;
	if (c == '.')
		return 39;
	return -1;
}

static char val_addr(int v)
{
	if (v == 0)
		return ' ';
	if (v >= 1 && v <= 26)
		return (char)('A' + v - 1);
	if (v >= 27 && v <= 36)
		return (char)('0' + v - 27);
	if (v == 37)
		return '-';
	if (v == 38)
		return '/';
	if (v == 39)
		return '.';
	return '\0';
}

static uint64_t pow40(unsigned n)
{
	uint64_t p = 1;

	while (n--)
		p *= 40u;
	return p;
}

bool m17_addr_encode(const char *text, uint8_t out[M17_ADDR_BYTES])
{
	uint64_t v = 0;
	unsigned n = 0;

	if (!text)
		return false;
	for (const char *p = text; *p; p++) {
		if (n >= M17_ADDR_CHARS_MAX)
			return false;
		int d = addr_val(*p);

		if (d < 0)
			return false;
		v += (uint64_t)d * pow40(n);
		n++;
	}
	for (uint8_t i = 0; i < M17_ADDR_BYTES; i++)
		out[i] = (uint8_t)(v >> (8 * (M17_ADDR_BYTES - 1 - i)));
	return true;
}

bool m17_addr_decode(const uint8_t in[M17_ADDR_BYTES], char *out, size_t outsz)
{
	uint64_t v = 0;

	if (!out || outsz < 2)
		return false;
	for (uint8_t i = 0; i < M17_ADDR_BYTES; i++)
		v = (v << 8) | in[i];
	if (v == M17_ADDR_BROADCAST) {
		if (outsz < 4)
			return false;
		strcpy(out, "ALL");
		return true;
	}
	unsigned n = 0;
	char rev[M17_ADDR_CHARS_MAX + 1];
	while (v && n < sizeof(rev) - 1) {
		char c = val_addr((int)(v % 40u));

		if (!c)
			break;
		rev[n++] = c;
		v /= 40u;
	}
	if (n >= outsz)
		return false;
	for (unsigned i = 0; i < n; i++)
		out[i] = rev[i];
	out[n] = '\0';
	return true;
}

// ── PRBS9 (BERT) ──
static void prbs9_bits(uint8_t *bits, uint16_t n, uint16_t seed)
{
	uint16_t s = seed ? seed : 0x1FF;

	for (uint16_t i = 0; i < n; i++) {
		bit_put(bits, i, (uint8_t)(s & 1));
		s = (uint16_t)((s >> 1) | (((s >> 4) ^ s) & 1) << 8);
	}
}

// Packed scratch for one coded block. fec_conv_encode and puncture_bits store one bit per bit (bit_put);
// the LSF path is the longest, 488 bits.
#define M17_CODED_BYTES	((M17_LSF_CODED_BITS + 7) / 8)

_Static_assert(M17_LSF_CODED_BITS <= M17_CODED_BYTES * 8 && M17_STREAM_CODED_BITS <= M17_CODED_BYTES * 8 &&
		       M17_PACKET_CODED_BITS <= M17_CODED_BYTES * 8 && M17_BERT_CODED_BITS <= M17_CODED_BYTES * 8,
	       "each path's coded bits fit the packed scratch");
_Static_assert(sizeof(((m17_fec_work_t *)0)->soft) >= 2 * 244, "soft holds a full LSF decode");

// ── Convolutional payload into 368-bit plain (before interleave) ──
static void conv_plain(const m17_conv_path_t *path, const uint8_t *in, uint16_t dest_off, uint8_t *plain368)
{
	uint8_t enc[M17_CODED_BYTES];
	uint8_t keep[M17_CODED_BYTES];

	fec_conv_encode(&M17_CONV_CODE, in, path->nbits, enc);
	puncture_bits(path, enc, keep);
	for (uint16_t i = 0; i < path->nkeep; i++)
		bit_put(plain368, (uint16_t)(dest_off + i), bit_get(keep, i));
}

static int conv_decode(const m17_conv_path_t *path, const uint8_t *plain368, uint16_t src_off, m17_fec_work_t *w,
		       uint8_t *out)
{
	uint8_t keep[M17_CODED_BYTES];

	for (uint16_t i = 0; i < path->nkeep; i++)
		bit_put(keep, i, bit_get(plain368, (uint16_t)(src_off + i)));
	unpuncture_soft(path, keep, path->nkeep, w->soft);
	return fec_conv_decode(&M17_CONV_CODE, w->soft, path->nbits, w->dec, out);
}

static const m17_conv_path_t path_lsf = { punct_p1, M17_PUNCTURE_P1_LEN, 244, M17_LSF_CODED_BITS, M17_PAYLOAD_BITS };
static const m17_conv_path_t path_stream = { punct_p2, M17_PUNCTURE_P2_LEN, 148, M17_STREAM_CODED_BITS,
					     M17_STREAM_AIR_BITS };
static const m17_conv_path_t path_packet = { punct_p3, M17_PUNCTURE_P3_LEN, 210, M17_PACKET_CODED_BITS, M17_PAYLOAD_BITS };
static const m17_conv_path_t path_bert = { punct_p2, M17_PUNCTURE_P2_LEN, 201, M17_BERT_CODED_BITS, M17_PAYLOAD_BITS };

// ── LSF ──
void m17_lsf_encode(uint8_t lsf[M17_LSF_BYTES], uint8_t payload[M17_PAYLOAD_BYTES])
{
	uint16_t crc;
	uint8_t in[32] = { 0 };
	uint8_t plain[M17_PAYLOAD_BYTES];

	crc = m17_crc16(lsf, M17_LSF_DATA_BYTES);
	lsf[28] = (uint8_t)(crc >> 8);
	lsf[29] = (uint8_t)crc;
	bits_from_bytes(lsf, 240, in);
	memset(plain, 0, sizeof(plain));
	conv_plain(&path_lsf, in, 0, plain);
	airify(plain);
	pack_bits(plain, payload);
}

int m17_lsf_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t lsf[M17_LSF_BYTES])
{
	uint8_t plain[M17_PAYLOAD_BYTES];
	uint8_t dec[32];
	uint16_t rx_crc, calc;
	int errs;

	deairify(payload, plain);
	errs = conv_decode(&path_lsf, plain, 0, w, dec);
	bytes_from_bits(dec, 240, lsf);
	rx_crc = (uint16_t)((lsf[28] << 8) | lsf[29]);
	calc = m17_crc16(lsf, M17_LSF_DATA_BYTES);
	return calc == rx_crc ? errs : -1;
}

// ── Stream ──
static void lich_bits(const uint8_t lsf[M17_LSF_BYTES], uint8_t counter, uint8_t *lich48)
{
	uint8_t chunk[5];

	memset(lich48, 0, 6);
	memcpy(chunk, lsf + (size_t)counter * 5, 5);
	for (uint16_t i = 0; i < 40; i++)
		bit_put(lich48, i, bit_get(chunk, i));
	for (uint8_t i = 0; i < 3; i++)
		bit_put(lich48, (uint16_t)(40 + i), (uint8_t)((counter >> (2 - i)) & 1));
}

static void golay_lich(const uint8_t lich48[6], uint8_t *out96)
{
	for (uint8_t w = 0; w < 4; w++) {
		uint16_t data = 0;

		for (uint8_t i = 0; i < 12; i++)
			data = (uint16_t)(data << 1 | bit_get(lich48, (uint16_t)(w * 12 + i)));
		uint32_t cw = fec_golay24_encode(data);

		for (uint8_t i = 0; i < 24; i++)
			bit_put(out96, (uint16_t)(w * 24 + i), (uint8_t)((cw >> (23 - i)) & 1));
	}
}

static int degolay_lich(const uint8_t *in96, uint8_t lich48[6])
{
	int total = 0;

	for (uint8_t w = 0; w < 4; w++) {
		uint32_t cw = 0;
		uint16_t data;
		int e;

		for (uint8_t i = 0; i < 24; i++)
			cw = (cw << 1) | bit_get(in96, (uint16_t)(w * 24 + i));
		e = fec_golay24_decode(cw, &data);
		if (e < 0)
			return -1;
		total += e;
		for (uint8_t i = 0; i < 12; i++)
			bit_put(lich48, (uint16_t)(w * 12 + i), (uint8_t)((data >> (11 - i)) & 1));
	}
	return total;
}

void m17_stream_encode(const uint8_t lsf[M17_LSF_BYTES], uint8_t lich_count, uint16_t frame_num,
		       const uint8_t stream_pl[M17_STREAM_VOICE_BITS / 8], uint8_t payload[M17_PAYLOAD_BYTES])
{
	uint8_t lich48[8] = { 0 };
	uint8_t lich96[12] = { 0 };
	uint8_t in[20] = { 0 };
	uint8_t plain[M17_PAYLOAD_BYTES];

	lich_bits(lsf, lich_count, lich48);
	golay_lich(lich48, lich96);
	for (uint8_t i = 0; i < 16; i++)
		bit_put(in, i, (uint8_t)((frame_num >> (15 - i)) & 1));
	for (uint16_t i = 0; i < M17_STREAM_VOICE_BITS; i++)
		bit_put(in, (uint16_t)(16 + i), bit_get(stream_pl, i));
	memset(plain, 0, sizeof(plain));
	for (uint16_t i = 0; i < 96; i++)
		bit_put(plain, i, bit_get(lich96, i));
	conv_plain(&path_stream, in, 96, plain);
	airify(plain);
	pack_bits(plain, payload);
}

int m17_stream_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t *lich_count,
		      uint8_t lich_chunk[5], uint16_t *frame_num, uint8_t stream_pl[M17_STREAM_VOICE_BITS / 8],
		      int *lich_errs)
{
	uint8_t plain[M17_PAYLOAD_BYTES];
	uint8_t dec[20];
	uint8_t lich48[8];
	uint8_t lich96[12];
	int errs;

	deairify(payload, plain);
	memset(lich96, 0, sizeof(lich96));
	for (uint16_t i = 0; i < 96; i++)
		bit_put(lich96, i, bit_get(plain, i));
	*lich_errs = degolay_lich(lich96, lich48);
	if (*lich_errs < 0)
		return -1;
	bytes_from_bits(lich48, 40, lich_chunk);
	*lich_count = 0;
	for (uint8_t i = 0; i < 3; i++)
		*lich_count = (uint8_t)(*lich_count << 1 | bit_get(lich48, (uint16_t)(40 + i)));
	errs = conv_decode(&path_stream, plain, 96, w, dec);
	if (errs < 0)
		return errs;
	*frame_num = 0;
	for (uint8_t i = 0; i < 16; i++)
		*frame_num = (uint16_t)(*frame_num << 1 | bit_get(dec, i));
	memset(stream_pl, 0, M17_STREAM_VOICE_BITS / 8);
	for (uint16_t i = 0; i < M17_STREAM_VOICE_BITS; i++)
		bit_put(stream_pl, i, bit_get(dec, (uint16_t)(16 + i)));
	return errs;
}

void m17_stream_lich_words(const uint8_t payload[M17_PAYLOAD_BYTES], uint32_t words[4])
{
	uint8_t plain[M17_PAYLOAD_BYTES];

	deairify(payload, plain);
	for (uint8_t w = 0; w < 4; w++) {
		uint32_t cw = 0;

		for (uint8_t i = 0; i < 24; i++)
			cw = (cw << 1) | bit_get(plain, (uint16_t)(w * 24 + i));
		words[w] = cw;
	}
}

// ── Packet ──
void m17_packet_pack(const uint8_t data[25], uint8_t frame_num, bool last, uint8_t valid_bytes, uint8_t bits[26])
{
	uint8_t field5;

	memset(bits, 0, 26);
	bits_from_bytes(data, 200, bits);
	bit_put(bits, 200, last ? 1 : 0);
	field5 = last ? valid_bytes : frame_num;
	for (uint8_t i = 0; i < 5; i++)
		bit_put(bits, (uint16_t)(201 + i), (uint8_t)((field5 >> (4 - i)) & 1));
}

void m17_packet_unpack(const uint8_t bits[26], uint8_t data[25], uint8_t *frame_num, bool *last,
		       uint8_t *valid_bytes)
{
	uint8_t field5 = 0;

	bytes_from_bits(bits, 200, data);
	*last = bit_get(bits, 200) != 0;
	for (uint8_t i = 0; i < 5; i++)
		field5 = (uint8_t)(field5 << 1 | bit_get(bits, (uint16_t)(201 + i)));
	*frame_num = *last ? 0 : field5;
	*valid_bytes = *last ? field5 : 0;
}

void m17_packet_encode(const uint8_t data[25], uint8_t frame_num, bool last, uint8_t valid_bytes,
		       uint8_t payload[M17_PAYLOAD_BYTES])
{
	uint8_t in[28] = { 0 };
	uint8_t plain[M17_PAYLOAD_BYTES];

	m17_packet_pack(data, frame_num, last, valid_bytes, in);
	memset(plain, 0, sizeof(plain));
	conv_plain(&path_packet, in, 0, plain);
	airify(plain);
	pack_bits(plain, payload);
}

int m17_packet_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t data[25],
		      uint8_t *frame_num, bool *last, uint8_t *valid_bytes)
{
	uint8_t plain[M17_PAYLOAD_BYTES];
	uint8_t dec[28];
	int errs;

	deairify(payload, plain);
	errs = conv_decode(&path_packet, plain, 0, w, dec);
	if (errs < 0)
		return errs;
	m17_packet_unpack(dec, data, frame_num, last, valid_bytes);
	return errs;
}

// ── BERT ──
void m17_bert_encode_bits(const uint8_t bits[], uint8_t payload[M17_PAYLOAD_BYTES])
{
	uint8_t in[28];
	uint8_t plain[M17_PAYLOAD_BYTES];
	uint8_t enc[M17_CODED_BYTES];
	uint8_t keep[M17_CODED_BYTES];

	memset(in, 0, sizeof in);
	for (uint16_t i = 0; i < M17_BERT_BITS; i++)
		bit_put(in, i, bit_get(bits, i));
	memset(plain, 0, sizeof plain);
	fec_conv_encode(&M17_CONV_CODE, in, path_bert.nbits, enc);
	puncture_bits(&path_bert, enc, keep);
	for (uint16_t i = 0; i < M17_PAYLOAD_BITS; i++)
		bit_put(plain, i, bit_get(keep, i));
	airify(plain);
	pack_bits(plain, payload);
}

void m17_bert_encode(uint8_t payload[M17_PAYLOAD_BYTES])
{
	uint8_t in[28] = { 0 };

	prbs9_bits(in, M17_BERT_BITS, 0x1FF);
	m17_bert_encode_bits(in, payload);
}

int m17_bert_decode(const uint8_t payload[M17_PAYLOAD_BYTES], m17_fec_work_t *w, uint8_t out[M17_BERT_BITS / 8])
{
	uint8_t plain[M17_PAYLOAD_BYTES];
	uint8_t dec[28];
	int errs;

	deairify(payload, plain);
	errs = conv_decode(&path_bert, plain, 0, w, dec);
	if (errs < 0)
		return errs;
	memset(out, 0, M17_BERT_BITS / 8);
	for (uint16_t i = 0; i < M17_BERT_BITS; i++)
		bit_put(out, i, bit_get(dec, i));
	return errs;
}

// Test helpers exported for puncture/interleave checks
uint16_t m17_puncture(const uint8_t *pat, uint8_t plen, const uint8_t *coded, uint16_t ncoded, uint8_t *out)
{
	m17_conv_path_t p = { pat, plen, 0, ncoded, 0 };

	return puncture_bits(&p, coded, out);
}

uint16_t m17_puncture_drop_last(const uint8_t *pat, uint8_t plen, const uint8_t *coded, uint16_t ncoded, uint8_t *out)
{
	uint8_t tmp[M17_CODED_BYTES];
	uint16_t n = m17_puncture(pat, plen, coded, ncoded, tmp);

	for (uint16_t i = 0; i + 1 < n; i++)
		bit_put(out, i, bit_get(tmp, i));
	return (uint16_t)(n - 1);
}

void m17_interleave(const uint8_t *in, uint8_t *out)
{
	interleave_bits(in, out);
}

void m17_deinterleave(const uint8_t *in, uint8_t *out)
{
	deinterleave_bits(in, out);
}
