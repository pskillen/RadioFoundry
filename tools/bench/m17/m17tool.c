// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 bench tool on the firmware's frame codec (core/m17). tools/bench/m17tool.py is the front end.
//
//   m17tool selftest
//   m17tool gen OUT.dibits --stream|--bert|--packet --src CALL --dst CALL [--can N] [--text MSG | --data-hex HEX]
//   m17tool decode IN.dibits
//   m17tool decode-message IN.dibits [--expect-src CALL] [--expect-dst CALL] [--expect-can N]
//
// gen writes packed dibits (four symbols per byte, hal/hal_air.h). Not one byte per symbol.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "air/air_syms.h"
#include "m17/m17.h"
#include "m17/m17_engine.h"
#include "m17/m17_packet.h"

#define M17_FRAME_BYTES	48
#define M17_PREAMBLE_BYTES	48
#define M17_END_BYTES	48
#define M17_MSG_TYPE	0x05u
#define M17_TEXT_BYTES_MAX	821u

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

static uint64_t addr_u64(const uint8_t raw[M17_ADDR_BYTES])
{
	uint64_t a = 0;

	for (unsigned i = 0; i < M17_ADDR_BYTES; i++)
		a = (a << 8) | raw[i];
	return a;
}

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool parse_hex(const char *text, uint8_t *out, size_t max, size_t *len)
{
	size_t n = 0;
	int hi = -1;

	for (const char *p = text; *p; p++) {
		int d;

		if (*p == ':' || *p == '-' || *p == ' ')
			continue;
		d = hex_nibble(*p);
		if (d < 0)
			return false;
		if (hi < 0)
			hi = d;
		else {
			if (n >= max)
				return false;
			out[n++] = (uint8_t)((hi << 4) | d);
			hi = -1;
		}
	}
	if (hi >= 0)
		return false;
	*len = n;
	return true;
}

static void print_hex(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", p[i]);
	printf("\n");
}

static int cmd_gen(int argc, char **argv)
{
	const char *path = NULL;
	bool stream = false, bert = false, packet = false;
	const char *src = "AB1CD", *dst = "ALL";
	const char *text = NULL, *data_hex = NULL;
	unsigned can = 0, type = M17_MSG_TYPE;
	unsigned frames = 1;
	int i0 = (argc > 0 && !strcmp(argv[0], "gen")) ? 1 : 0;

	for (int i = i0; i < argc; i++) {
		if (!strcmp(argv[i], "--stream"))
			stream = true;
		else if (!strcmp(argv[i], "--bert"))
			bert = true;
		else if (!strcmp(argv[i], "--packet"))
			packet = true;
		else if (!strcmp(argv[i], "--src") && i + 1 < argc)
			src = argv[++i];
		else if (!strcmp(argv[i], "--dst") && i + 1 < argc)
			dst = argv[++i];
		else if (!strcmp(argv[i], "--can") && i + 1 < argc)
			can = (unsigned)atoi(argv[++i]);
		else if (!strcmp(argv[i], "--type") && i + 1 < argc)
			type = (unsigned)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--text") && i + 1 < argc)
			text = argv[++i];
		else if (!strcmp(argv[i], "--data-hex") && i + 1 < argc)
			data_hex = argv[++i];
		else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
			frames = (unsigned)atoi(argv[++i]);
		else if (!path)
			path = argv[i];
	}
	if (!path || (stream + bert + packet != 1))
		die("usage: gen OUT.dibits (--stream|--bert|--packet) "
		    "[--src CALL] [--dst CALL] [--can N] [--text MSG | --data-hex HEX] [--frames N]");
	if (packet) {
		uint8_t payload[M17_PACKET_MAX_BYTES];
		uint8_t src_b[M17_ADDR_BYTES], dst_b[M17_ADDR_BYTES];
		size_t len = 0;
		m17_engine_t e;
		m17_engine_cfg_t cfg;
		uint8_t buf[(M17_PACKET_MAX_FRAMES + 3) * M17_FRAME_BYTES];
		uint16_t got;
		FILE *f;

		if (can > 15)
			die("CAN out of range");
		if (data_hex) {
			if (!parse_hex(data_hex, payload, sizeof payload, &len))
				die("bad --data-hex");
		} else if (text) {
			len = strlen(text);
			if (len > M17_TEXT_BYTES_MAX)
				die("text too long (821 UTF-8 bytes max before the final NUL)");
			memcpy(payload, text, len);
			if (!len || payload[len - 1] != 0)
				payload[len++] = 0;
		} else {
			die("packet needs --text or --data-hex");
		}
		if (!m17_packet_fits((uint16_t)len))
			die("packet does not fit");
		if (!addr_set(dst_b, dst) || !addr_set(src_b, src))
			die("bad address");
		memset(&cfg, 0, sizeof cfg);
		cfg.own = addr_u64(src_b);
		cfg.dst = addr_u64(dst_b);
		cfg.can = (uint8_t)can;
		cfg.tx_kind = M17_TX_PACKET;
		m17_engine_init(&e, &cfg, NULL, NULL);
		if (!m17_engine_tx_start(&e, (uint8_t)type, payload, (uint16_t)len))
			die("packet refused");
		got = m17_engine_tx_read(&e, buf, sizeof buf);
		if (!got)
			die("packet produced no output");
		f = fopen(path, "wb");
		if (!f)
			die("can't open output");
		if (fwrite(buf, 1, got, f) != got)
			die("write failed");
		fclose(f);
		return 0;
	}
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
	if ((fr[0] == SYNC_LSF0 && fr[1] == SYNC_LSF1) || (fr[0] == 0x55u && fr[1] == 0xF7u))
		return FT_LSF;
	if (fr[0] == SYNC_STREAM0 && fr[1] == SYNC_STREAM1)
		return FT_STREAM;
	if (fr[0] == SYNC_BERT0 && fr[1] == SYNC_BERT1)
		return FT_BERT;
	if (fr[0] == SYNC_PACKET0 && fr[1] == SYNC_PACKET1)
		return FT_PACKET;
	return FT_NONE;
}

static bool load_file(const char *path, uint8_t **raw, size_t *n)
{
	FILE *f = fopen(path, "rb");

	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	*n = (size_t)ftell(f);
	rewind(f);
	*raw = malloc(*n);
	if (!*raw)
		die("out of memory");
	if (fread(*raw, 1, *n, f) != *n)
		die("read failed");
	fclose(f);
	return true;
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
	if (!load_file(path, &raw, &n))
		die("can't open input");
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

static void msg_kv(const char *key, const char *fmt, ...)
{
	va_list ap;

	printf("m17-message/1 %s=", key);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static uint8_t lsf_can(const uint8_t lsf[M17_LSF_BYTES])
{
	uint16_t type = (uint16_t)((lsf[M17_LSF_TYPE_OFFSET] << 8) | lsf[M17_LSF_TYPE_OFFSET + 1]);

	return (uint8_t)((type & M17_TYPE_CAN_MASK) >> M17_TYPE_CAN_SHIFT);
}

static int cmd_decode_message(int argc, char **argv)
{
	int i0 = (argc > 0 && !strcmp(argv[0], "decode-message")) ? 1 : 0;
	const char *path = NULL;
	const char *expect_src = NULL, *expect_dst = NULL;
	int expect_can = -1;
	uint8_t *raw;
	size_t n, off;
	m17_fec_work_t w;
	m17_packet_gather_t g;
	uint8_t lsf[M17_LSF_BYTES];
	bool fresh_lsf = false, saw_packet = false, complete = false, saw_eot = false;
	unsigned fec_disagreements = 0, packet_frames = 0;
	unsigned field_disagreements = 0;
	const char *fail = "incomplete";

	for (int i = i0; i < argc; i++) {
		if (!strcmp(argv[i], "--expect-src") && i + 1 < argc)
			expect_src = argv[++i];
		else if (!strcmp(argv[i], "--expect-dst") && i + 1 < argc)
			expect_dst = argv[++i];
		else if (!strcmp(argv[i], "--expect-can") && i + 1 < argc)
			expect_can = atoi(argv[++i]);
		else if (!path)
			path = argv[i];
	}
	if (!path)
		die("usage: decode-message IN.dibits [--expect-src CALL] [--expect-dst CALL] [--expect-can N]");
	if (!load_file(path, &raw, &n))
		die("can't open input");
	m17_packet_gather_init(&g);
	if (n < M17_PREAMBLE_BYTES) {
		fail = "too short";
		goto out;
	}
	off = M17_PREAMBLE_BYTES;
	while (off + M17_FRAME_BYTES <= n) {
		const uint8_t *fr = raw + off;
		frame_kind_t k = classify(fr);
		const uint8_t *pay = fr + 2;

		if (k == FT_END) {
			saw_eot = true;
			break;
		}
		if (k == FT_LSF) {
			uint16_t type;
			int r = m17_lsf_decode(pay, &w, lsf);

			if (r < 0) {
				fail = "lsf crc";
				goto out;
			}
			type = (uint16_t)((lsf[M17_LSF_TYPE_OFFSET] << 8) | lsf[M17_LSF_TYPE_OFFSET + 1]);
			if (type & M17_TYPE_STREAM) {
				fail = "stream lsf";
				goto out;
			}
			fresh_lsf = true;
			m17_packet_gather_init(&g);
			saw_packet = false;
		} else if (k == FT_PACKET) {
			uint8_t data[M17_PACKET_FRAME_BYTES];
			uint8_t fn, vb;
			bool last;
			int rc;

			if (!fresh_lsf) {
				fail = "packet before lsf";
				goto out;
			}
			rc = m17_packet_decode(pay, &w, data, &fn, &last, &vb);
			if (rc < 0) {
				fail = "packet decode";
				goto out;
			}
			fec_disagreements += (unsigned)rc;
			saw_packet = true;
			packet_frames++;
			rc = m17_packet_gather_frame(&g, data, last, vb, fn);
			if (rc < 0) {
				fail = "packet continuity";
				goto out;
			}
			if (rc == 1)
				complete = true;
		} else if (k != FT_NONE && fresh_lsf) {
			fail = "foreign frame";
			goto out;
		}
		off += M17_FRAME_BYTES;
	}
	if (!fresh_lsf)
		fail = "no lsf";
	else if (!saw_packet)
		fail = "no packet";
	else if (!complete)
		fail = "incomplete packet";
	else if (!saw_eot)
		fail = "no eot";
	else if (g.buf[0] != M17_MSG_TYPE)
		fail = "type";
	else if (g.len < 4)
		fail = "short packet";
	else if (g.buf[g.len - 3] != 0)
		fail = "missing nul";

	if (!strcmp(fail, "incomplete") && complete && saw_eot && g.buf[0] == M17_MSG_TYPE && g.len >= 4 &&
	    g.buf[g.len - 3] == 0) {
		char src[16], dst[16];
		uint8_t exp_raw[M17_ADDR_BYTES];
		unsigned disagreements = fec_disagreements;

		if (!m17_addr_decode(lsf + M17_ADDR_BYTES, src, sizeof src))
			strncpy(src, "?", sizeof src);
		if (!m17_addr_decode(lsf, dst, sizeof dst))
			strncpy(dst, "?", sizeof dst);
		if (expect_src) {
			if (!m17_addr_encode(expect_src, exp_raw) ||
			    memcmp(lsf + M17_ADDR_BYTES, exp_raw, M17_ADDR_BYTES))
				field_disagreements++;
		}
		if (expect_dst) {
			if (!addr_set(exp_raw, expect_dst) || memcmp(lsf, exp_raw, M17_ADDR_BYTES))
				field_disagreements++;
		}
		if (expect_can >= 0 && lsf_can(lsf) != (uint8_t)expect_can)
			field_disagreements++;
		disagreements += field_disagreements;
		msg_kv("ok", "%u", field_disagreements == 0 ? 1u : 0u);
		msg_kv("src", "%s", src);
		msg_kv("dst", "%s", dst);
		msg_kv("can", "%u", (unsigned)lsf_can(lsf));
		msg_kv("type", "%02x", (unsigned)g.buf[0]);
		msg_kv("frames", "%u", packet_frames);
		msg_kv("bytes", "%u", (unsigned)(g.len - 3));
		msg_kv("eot", "%u", 1u);
		msg_kv("crc", "%s", "ok");
		printf("m17-message/1 packet_hex=");
		print_hex(g.buf, g.len);
		printf("m17-message/1 src_raw=");
		print_hex(lsf + M17_ADDR_BYTES, M17_ADDR_BYTES);
		printf("m17-message/1 dst_raw=");
		print_hex(lsf, M17_ADDR_BYTES);
		printf("m17-message/1 text_hex=");
		print_hex(g.buf + 1, g.len - 3);
		msg_kv("fec_disagreements", "%u", fec_disagreements);
		msg_kv("field_disagreements", "%u", field_disagreements);
		msg_kv("disagreements", "%u", disagreements);
		if (expect_src)
			msg_kv("expect_src", "%s", expect_src);
		if (expect_dst)
			msg_kv("expect_dst", "%s", expect_dst);
		if (expect_can >= 0)
			msg_kv("expect_can", "%d", expect_can);
		free(raw);
		return field_disagreements == 0 ? 0 : 1;
	}
out:
	msg_kv("ok", "0");
	msg_kv("reason", "%s", fail);
	msg_kv("fec_disagreements", "%u", fec_disagreements);
	msg_kv("field_disagreements", "%u", field_disagreements);
	msg_kv("disagreements", "%u", fec_disagreements + field_disagreements);
	free(raw);
	return 1;
}

int main(int argc, char **argv)
{
	if (argc < 2)
		die("usage: m17tool selftest | gen ... | decode IN.dibits | decode-message IN.dibits");
	if (!strcmp(argv[1], "selftest"))
		return cmd_selftest();
	if (!strcmp(argv[1], "gen"))
		return cmd_gen(argc - 1, argv + 1);
	if (!strcmp(argv[1], "decode"))
		return cmd_decode(argc - 1, argv + 1);
	if (!strcmp(argv[1], "decode-message"))
		return cmd_decode_message(argc - 1, argv + 1);
	die("unknown command");
	return 2;
}