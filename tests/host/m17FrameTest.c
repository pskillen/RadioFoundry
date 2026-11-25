// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 frame codec: CRC check values, addresses, interleaver, puncturing, Golay rows, and round trips.
#include <stdio.h>
#include <string.h>

#include "fec/golay.h"
#include "m17/m17.h"

static int failures, checks;
#define CHECK(c, ...)                                                                                                          \
	do {                                                                                                                 \
		checks++;                                                                                                      \
		if (!(c)) {                                                                                                    \
			failures++;                                                                                            \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                            \
			printf(__VA_ARGS__);                                                                                   \
			printf("\n");                                                                                          \
		}                                                                                                              \
	} while (0)

static void test_crc(void)
{
	CHECK(m17_crc16((const uint8_t *)"", 0) == 0xFFFF, "empty");
	CHECK(m17_crc16((const uint8_t *)"A", 1) == 0x206E, "A");
	CHECK(m17_crc16((const uint8_t *)"123456789", 9) == 0x772B, "123456789");
	{
		uint8_t all[256];
		for (int i = 0; i < 256; i++)
			all[i] = (uint8_t)i;
		CHECK(m17_crc16(all, 256) == 0x1C31, "0..255");
	}
	{
		uint8_t lsf[M17_LSF_BYTES];
		uint8_t pay[M17_PAYLOAD_BYTES];
		m17_fec_work_t w;

		memset(lsf, 0xA5, sizeof(lsf));
		m17_lsf_encode(lsf, pay);
		CHECK(m17_crc16(lsf, 30) == 0, "LSF post-encode CRC");
		CHECK(m17_lsf_decode(pay, &w, lsf) >= 0, "LSF CRC");
	}
}

static void test_addr(void)
{
	uint8_t b[M17_ADDR_BYTES];
	char text[16];

	CHECK(m17_addr_encode("AB1CD", b), "AB1CD encode");
	CHECK(m17_addr_decode(b, text, sizeof(text)), "AB1CD decode");
	CHECK(!strcmp(text, "AB1CD"), "AB1CD text");
	CHECK(b[3] == 0x9F && b[4] == 0xDD && b[5] == 0x51, "AB1CD bytes");
	CHECK(m17_addr_encode("ABC", b) && m17_addr_encode("ABC   ", b), "spaces");
	{
		uint8_t a[M17_ADDR_BYTES], c[M17_ADDR_BYTES];
		CHECK(m17_addr_encode("abc", a), "lower");
		CHECK(m17_addr_encode("ABC", c), "upper");
		CHECK(!memcmp(a, c, M17_ADDR_BYTES), "lower=upper");
	}
	CHECK(m17_addr_encode("123456789", b), "9 chars");
	CHECK(!m17_addr_encode("1234567890", b), "10 chars");
	CHECK(!m17_addr_encode("AB@CD", b), "@");
	{
		uint8_t bc[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
		CHECK(m17_addr_decode(bc, text, sizeof(text)), "broadcast");
		CHECK(!strcmp(text, "ALL"), "ALL");
	}
}

static void test_interleave(void)
{
	uint8_t in[46], out[46];
	uint16_t seen[368];

	memset(in, 0xAA, sizeof(in));
	memset(seen, 0, sizeof(seen));
	m17_interleave(in, out);
	for (uint16_t x = 0; x < M17_PAYLOAD_BITS; x++) {
		uint16_t y = m17_interleave_qpp(x);
		CHECK(!seen[y], "perm %u", y);
		seen[y] = 1;
	}
	CHECK(m17_interleave_qpp(1) == 137 && m17_interleave_qpp(2) == 90 && m17_interleave_qpp(3) == 227 &&
	      m17_interleave_qpp(45) == 277 && m17_interleave_qpp(137) == 1, "spots");
}

static const uint8_t p1[M17_PUNCTURE_P1_LEN] = {
	1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
	1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, 0, 1, 1,
};
static const uint8_t p2[M17_PUNCTURE_P2_LEN] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0 };
static const uint8_t p3[M17_PUNCTURE_P3_LEN] = { 1, 1, 1, 1, 1, 1, 1, 0 };

static void test_puncture(void)
{
	uint8_t coded[512], out[400];

	memset(coded, 0xFF, sizeof(coded));
	CHECK(m17_puncture(p1, sizeof(p1), coded, 488, out) == 368, "488->368");
	CHECK(m17_puncture(p2, sizeof(p2), coded, 296, out) == 272, "296->272");
	CHECK(m17_puncture(p3, sizeof(p3), coded, 420, out) == 368, "420->368");
	CHECK(m17_puncture(p2, sizeof(p2), coded, 402, out) == 369, "402->369");
	CHECK(m17_puncture_drop_last(p2, sizeof(p2), coded, 402, out) == 368, "402->368");
}

static void test_golay_rows(void)
{
	static const uint16_t expect[12] = { 0xC75, 0x63B, 0xF68, 0x7B4, 0x3DA, 0xD99,
					     0x6CD, 0x367, 0xDC6, 0xA97, 0x93E, 0x8EB };

	for (uint8_t i = 0; i < 12; i++) {
		uint16_t data = (uint16_t)(0x800u >> i);
		CHECK(fec_golay24_encode(data) == ((uint32_t)data << 12 | expect[i]), "golay row %u", i);
	}
}

static void flip_payload_bit(uint8_t *pay, uint16_t bit)
{
	pay[bit >> 3] ^= (uint8_t)(0x80u >> (bit & 7));
}

static uint16_t tx_bit_for_deint_pos(uint16_t deint_pos)
{
	uint8_t in[M17_PAYLOAD_BYTES], out[M17_PAYLOAD_BYTES];
	uint16_t i;

	memset(in, 0, sizeof(in));
	flip_payload_bit(in, deint_pos);
	m17_deinterleave(in, out);
	for (i = 0; i < M17_PAYLOAD_BITS; i++) {
		if (out[i >> 3] & (uint8_t)(0x80u >> (i & 7)))
			return i;
	}
	return 0;
}

static void test_lsf_roundtrip(void)
{
	uint8_t lsf[M17_LSF_BYTES], lsf2[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES];
	m17_fec_work_t w;
	int i;

	memset(lsf, 0x3C, sizeof(lsf));
	m17_lsf_encode(lsf, pay);
	CHECK(m17_lsf_decode(pay, &w, lsf2) >= 0, "clean");
	CHECK(!memcmp(lsf, lsf2, M17_LSF_BYTES), "match");
	m17_lsf_encode(lsf, pay);
	for (i = 0; i < M17_PAYLOAD_BITS; i++) {
		uint8_t p[M17_PAYLOAD_BYTES];
		uint8_t d[M17_LSF_BYTES];

		memcpy(p, pay, sizeof(p));
		flip_payload_bit(p, (uint16_t)i);
		CHECK(m17_lsf_decode(p, &w, d) >= 0, "flip %d", i);
		CHECK(!memcmp(lsf, d, M17_LSF_BYTES), "flip %d data", i);
	}
}

static void test_stream_roundtrip(void)
{
	uint8_t lsf[M17_LSF_BYTES], pl[16], pay[M17_PAYLOAD_BYTES], pl2[16], chunk[5];
	m17_fec_work_t w;
	uint8_t cnt;
	uint16_t fn;
	int le, dec_ret;
	uint16_t flip_lich, flip_conv;

	for (uint8_t i = 0; i < M17_LSF_BYTES; i++)
		lsf[i] = i;
	memset(pl, 0x5A, sizeof(pl));
	m17_stream_encode(lsf, 3, 7, pl, pay);
	flip_lich = tx_bit_for_deint_pos(10);
	flip_conv = tx_bit_for_deint_pos(200);
	flip_payload_bit(pay, flip_lich);
	flip_payload_bit(pay, flip_conv);
	dec_ret = m17_stream_decode(pay, &w, &cnt, chunk, &fn, pl2, &le);
	CHECK(dec_ret >= 1, "Viterbi corrected");
	CHECK(le == 1, "lich golay errs");
	CHECK(cnt == 3, "LICH 3");
	CHECK(fn == 7, "fn");
	CHECK(!memcmp(chunk, lsf + 15, 5), "lich chunk");
	CHECK(!memcmp(pl, pl2, sizeof(pl)), "payload");
}

static void test_packet_bert_roundtrip(void)
{
	uint8_t data[25], data2[25], bits[26], pay[M17_PAYLOAD_BYTES], bert[M17_BERT_BITS / 8], bert2[M17_BERT_BITS / 8];
	m17_fec_work_t w;
	uint8_t fnum, vb;
	bool last;

	memset(data, 0x42, sizeof(data));
	m17_packet_pack(data, 0, true, 25, bits);
	CHECK(bits[25] == 0xE4, "last pack byte");
	m17_packet_unpack(bits, data2, &fnum, &last, &vb);
	CHECK(!memcmp(data, data2, 25) && last && vb == 25 && !fnum, "last unpack");
	m17_packet_pack(data, 2, false, 0, bits);
	CHECK(bits[25] == 0x08, "mid pack byte");
	m17_packet_unpack(bits, data2, &fnum, &last, &vb);
	CHECK(!memcmp(data, data2, 25) && fnum == 2 && !last, "mid unpack");
	memset(data, 0x42, sizeof(data));
	m17_packet_encode(data, 2, false, 0, pay);
	CHECK(m17_packet_decode(pay, &w, data2, &fnum, &last, &vb) >= 0, "packet");
	CHECK(!memcmp(data, data2, 25) && fnum == 2 && !last, "packet data");
	memset(data, 0x33, sizeof(data));
	m17_packet_encode(data, 0, true, 7, pay);
	CHECK(m17_packet_decode(pay, &w, data2, &fnum, &last, &vb) >= 0, "last packet");
	CHECK(last && vb == 7, "last valid");
	m17_bert_encode(pay);
	CHECK(m17_bert_decode(pay, &w, bert2) >= 0, "bert");
	m17_bert_encode(pay);
	m17_bert_decode(pay, &w, bert);
	CHECK(!memcmp(bert, bert2, sizeof(bert)), "bert stable");
}

int main(void)
{
	test_crc();
	test_addr();
	test_interleave();
	test_puncture();
	test_golay_rows();
	test_lsf_roundtrip();
	test_stream_roundtrip();
	test_packet_bert_roundtrip();
	printf("m17FrameTest: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}