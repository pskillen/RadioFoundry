// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 bench tool on the firmware's frame codec (core/m17). tools/bench/m17tool.py is the front end.
//
//   m17tool selftest
//   m17tool gen OUT.dibits --stream|--bert --src CALL --dst CALL --frames N
//   m17tool decode IN.dibits
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_syms.h"
#include "m17/m17.h"

#define M17_FRAME_BYTES	48		// 192 symbols: 8 sync + 46 payload bytes
#define M17_PREAMBLE_BYTES	48	// 192 symbols
#define M17_END_BYTES	48

#define SYNC_LSF0	0x77u
#define SYNC_LSF1	0x77u
#define SYNC_STREAM0	0xFFu
#define SYNC_STREAM1	0x5Du
#define SYNC_BERT0	0xDFu
#define SYNC_BERT1	0x55u
#define SYNC_PACKET0	0x75u
#define SYNC_PACKET1	0xFFu
#define SYNC_END0	0x55u
#define SYNC_END1	0x5Du

static void die(const char *msg)
{
	fprintf(stderr, "m17tool: %s\n", msg);
	exit(2);
}

static void repeat_byte(uint8_t *out, size_t n, uint8_t b)
{
	for (size_t i = 0; i < n; i++)
		out[i] = b;
}

static void write_preamble(uint8_t *out, int bert)
{
	repeat_byte(out, M17_PREAMBLE_BYTES, bert ? 0xDDu : 0x77u);
}

static void write_end(uint8_t *out)
{
	for (size_t i = 0; i < M17_END_BYTES; i += 2) {
		out[i] = SYNC_END0;
		out[i + 1] = SYNC_END1;
	}
}

static void write_frame(uint8_t *out, uint8_t s0, uint8_t s1, const uint8_t pay[M17_PAYLOAD_BYTES])
{
	out[0] = s0;
	out[1] = s1;
	memcpy(out + 2, pay, M17_PAYLOAD_BYTES);
}

static int cmd_selftest(void)
{
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES], pl[16], pkt[25], chunk[5];
	m17_fec_work_t w;
	uint8_t cnt;
	uint16_t fn;
	int le, errs = 0;

	memset(lsf, 0x11, sizeof(lsf));
	m17_lsf_encode(lsf, pay);
	if (m17_lsf_decode(pay, &w, lsf) < 0) {
		fprintf(stderr, "m17tool: LSF round trip failed\n");
		errs++;
	}
	memset(pl, 0x5a, sizeof(pl));
	m17_stream_encode(lsf, 0, 0, pl, pay);
	if (m17_stream_decode(pay, &w, &cnt, chunk, &fn, pl, &le) < 0) {
		fprintf(stderr, "m17tool: stream round trip failed\n");
		errs++;
	}
	memset(pkt, 0x42, sizeof(pkt));
	m17_packet_encode(pkt, 0, false, 0, pay);
	{
		bool last;
		uint8_t vb;

		if (m17_packet_decode(pay, &w, pkt, &cnt, &last, &vb) < 0) {
		fprintf(stderr, "m17tool: packet round trip failed\n");
		errs++;
		}
	}
	m17_bert_encode(pay);
	if (m17_bert_decode(pay, &w, pkt) < 0) {
		fprintf(stderr, "m17tool: BERT round trip failed\n");
		errs++;
	}
	if (errs)
		return 1;
	printf("m17tool: selftest ok\n");
	return 0;
}

static bool addr_set(uint8_t dest[M17_ADDR_BYTES], const char *text)
{
	if (!text || !strcmp(text, "ALL"))
		memset(dest, 0xFF, M17_ADDR_BYTES);
	else
		return m17_addr_encode(text, dest);
	return true;
}

static int cmd_gen(int argc, char **argv)
{
	const char *path = NULL;
	bool stream = false, bert = false;
	const char *src = "AB1CD", *dst = "ALL";
	unsigned frames = 1;
	int i0 = (argc > 0 && !strcmp(argv[0], "gen")) ? 1 : 0;

	for (int i = i0; i < argc; i++) {
		if (!strcmp(argv[i], "--stream"))
			stream = true;
		else if (!strcmp(argv[i], "--bert"))
			bert = true;
		else if (!strcmp(argv[i], "--src") && i + 1 < argc)
			src = argv[++i];
		else if (!strcmp(argv[i], "--dst") && i + 1 < argc)
			dst = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = (unsigned)atoi(argv[++i]);
		else if (!path)
			path = argv[i];
	}
	if (!path || (!stream && !bert) || (stream && bert))
		die("usage: gen OUT.dibits (--stream|--bert) [--src CALL] [--dst CALL] [--frames N]");
	FILE *f = fopen(path, "wb");
	size_t need = M17_PREAMBLE_BYTES + (stream ? M17_FRAME_BYTES * (1 + frames) : M17_FRAME_BYTES * frames)
		    + M17_END_BYTES;
	uint8_t *out = calloc(need, 1);
	uint8_t lsf[M17_LSF_BYTES], pay[M17_PAYLOAD_BYTES], pl[16];
	uint8_t src_b[M17_ADDR_BYTES], dst_b[M17_ADDR_BYTES];
	size_t off = 0;

	if (!f || !out)
		die("out of memory");
	write_preamble(out + off, bert);
	off += M17_PREAMBLE_BYTES;
	if (stream) {
		if (!addr_set(dst_b, dst) || !addr_set(src_b, src))
			die("bad address");
		memset(lsf, 0, sizeof(lsf));
		memcpy(lsf, dst_b, M17_ADDR_BYTES);
		memcpy(lsf + 6, src_b, M17_ADDR_BYTES);
		m17_lsf_encode(lsf, pay);
		write_frame(out + off, SYNC_LSF0, SYNC_LSF1, pay);
		off += M17_FRAME_BYTES;
		memset(pl, 0xA5, sizeof(pl));
		for (unsigned n = 0; n < frames; n++) {
			m17_stream_encode(lsf, (uint8_t)(n % 6), (uint16_t)n, pl, pay);
			write_frame(out + off, SYNC_STREAM0, SYNC_STREAM1, pay);
			off += M17_FRAME_BYTES;
		}
	} else {
		for (unsigned n = 0; n < frames; n++) {
			m17_bert_encode(pay);
			write_frame(out + off, SYNC_BERT0, SYNC_BERT1, pay);
			off += M17_FRAME_BYTES;
		}
	}
	write_end(out + off);
	off += M17_END_BYTES;
	if (fwrite(out, 1, off, f) != off)
		die("write failed");
	fclose(f);
	free(out);
	return 0;
}

typedef enum { FT_NONE, FT_LSF, FT_STREAM, FT_BERT, FT_PACKET, FT_END } frame_kind_t;

static frame_kind_t classify(const uint8_t *fr)
{
	if (fr[0] == SYNC_END0 && fr[1] == SYNC_END1)
		return FT_END;
	if (fr[0] == SYNC_LSF0 && fr[1] == SYNC_LSF1)
		return FT_LSF;
	if (fr[0] == SYNC_STREAM0 && fr[1] == SYNC_STREAM1)
		return FT_STREAM;
	if (fr[0] == SYNC_BERT0 && fr[1] == SYNC_BERT1)
		return FT_BERT;
	if (fr[0] == SYNC_PACKET0 && fr[1] == SYNC_PACKET1)
		return FT_PACKET;
	return FT_NONE;
}

static int cmd_decode(int argc, char **argv)
{
	int i0 = (argc > 0 && !strcmp(argv[0], "decode")) ? 1 : 0;
	const char *path = i0 + 1 <= argc ? argv[i0] : NULL;
	uint8_t *raw;
	size_t n, off = 0;
	int decoded = 0, stream_frames = 0, bert_bits = 0;
	m17_fec_work_t w;

	if (!path)
		die("usage: decode IN.dibits");
	raw = malloc(1);
	if (!raw)
		die("out of memory");
	FILE *f = fopen(path, "rb");

	if (!f)
		die("can't open input");
	fseek(f, 0, SEEK_END);
	n = (size_t)ftell(f);
	rewind(f);
	raw = realloc(raw, n);
	if (fread(raw, 1, n, f) != n)
		die("read failed");
	fclose(f);
	if (n < M17_PREAMBLE_BYTES)
		die("too short");
	off = M17_PREAMBLE_BYTES;
	while (off + M17_FRAME_BYTES <= n) {
		frame_kind_t k = classify(raw + off);
		const uint8_t *pay = raw + off + 2;

		if (k == FT_END)
			break;
		if (k == FT_LSF) {
			uint8_t lsf[M17_LSF_BYTES];
			char dst[16], src[16];
			int r = m17_lsf_decode(pay, &w, lsf);

			printf("LSF CRC %s\n", r >= 0 ? "ok" : "fail");
			if (m17_addr_decode(lsf, dst, sizeof(dst)) && m17_addr_decode(lsf + 6, src, sizeof(src)))
				printf("LSF src %s dst %s\n", src, dst);
			decoded = 1;
		} else if (k == FT_STREAM) {
			uint8_t pl[16];
			uint8_t cnt, chunk[5];
			uint16_t fn;
			int le;

			if (m17_stream_decode(pay, &w, &cnt, chunk, &fn, pl, &le) >= 0) {
				printf("stream frame %u\n", (unsigned)fn);
				stream_frames++;
				decoded = 1;
			}
		} else if (k == FT_BERT) {
			uint8_t bits[M17_BERT_BITS / 8];

			if (m17_bert_decode(pay, &w, bits) >= 0) {
				bert_bits += M17_BERT_BITS;
				decoded = 1;
			}
		} else if (k == FT_PACKET) {
			uint8_t data[25];
			uint8_t fnum, vb;
			bool last;

			if (m17_packet_decode(pay, &w, data, &fnum, &last, &vb) >= 0)
				decoded = 1;
		}
		off += M17_FRAME_BYTES;
	}
	printf("summary: %d stream frames, %d BERT bits\n", stream_frames, bert_bits);
	free(raw);
	return decoded ? 0 : 1;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		die("usage: m17tool selftest | gen ... | decode IN.dibits");
	if (!strcmp(argv[1], "selftest"))
		return cmd_selftest();
	if (!strcmp(argv[1], "gen"))
		return cmd_gen(argc - 1, argv + 1);
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argc - 1, argv + 1);
	die("unknown command");
	return 2;
}