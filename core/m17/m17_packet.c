// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Packet bytes and the CRC over them (docs/spec/m17.md 3.3, 3.4). Frame bits stay in m17_packet_pack.
#include "m17_packet.h"

#if CONFIG_M17 && CONFIG_M17_PACKET

#include <string.h>

#include "m17.h"

bool m17_packet_fits(uint16_t data_len)
{
	uint32_t n = (uint32_t)data_len + 3u;
	uint32_t frames = (n + M17_PACKET_FRAME_BYTES - 1) / M17_PACKET_FRAME_BYTES;

	return frames >= 1 && frames <= M17_PACKET_MAX_FRAMES;
}

bool m17_packet_build(uint8_t type, const uint8_t *data, uint16_t data_len, uint8_t *out, uint16_t *out_len)
{
	uint16_t n;
	uint16_t crc;

	if (!m17_packet_fits(data_len))
		return false;
	n = (uint16_t)(data_len + 3u);
	out[0] = type;
	if (data_len)
		memcpy(out + 1, data, data_len);
	crc = m17_crc16(out, (uint16_t)(data_len + 1u));
	out[data_len + 1u] = (uint8_t)(crc >> 8);
	out[data_len + 2u] = (uint8_t)crc;
	*out_len = n;
	return true;
}

bool m17_packet_slice(const uint8_t *pkt, uint16_t len, uint8_t index, uint8_t data[M17_PACKET_FRAME_BYTES],
		      bool *last, uint8_t *valid, uint8_t *frame_num)
{
	uint16_t frames;
	uint16_t off;
	uint16_t n;

	if (!len)
		return false;
	frames = (uint16_t)((len + M17_PACKET_FRAME_BYTES - 1) / M17_PACKET_FRAME_BYTES);
	if (index >= frames)
		return false;
	off = (uint16_t)index * M17_PACKET_FRAME_BYTES;
	n = (uint16_t)(len - off);
	*last = index + 1u == frames;
	if (*last) {
		if (n > M17_PACKET_FRAME_BYTES)
			return false;
		*valid = (uint8_t)n;
		*frame_num = 0;
	} else {
		n = M17_PACKET_FRAME_BYTES;
		*valid = 0;
		*frame_num = index;
	}
	memset(data, 0, M17_PACKET_FRAME_BYTES);
	memcpy(data, pkt + off, n);
	return true;
}

void m17_packet_gather_init(m17_packet_gather_t *g)
{
	memset(g, 0, sizeof *g);
}

int m17_packet_gather_frame(m17_packet_gather_t *g, const uint8_t data[M17_PACKET_FRAME_BYTES], bool last,
			    uint8_t valid, uint8_t frame_num)
{
	uint16_t n;
	uint16_t got;
	uint16_t crc;

	if (last) {
		if (valid == 0 || valid > M17_PACKET_FRAME_BYTES)
			return -1;
		n = valid;
	} else {
		if (frame_num != g->next || frame_num > 31)
			return -1;
		n = M17_PACKET_FRAME_BYTES;
	}
	if ((uint32_t)g->len + n > M17_PACKET_MAX_BYTES)
		return -1;
	memcpy(g->buf + g->len, data, n);
	g->len = (uint16_t)(g->len + n);
	g->next++;
	g->started = true;
	if (!last)
		return 0;
	if (g->len < 3)
		return -1;
	got = (uint16_t)((g->buf[g->len - 2] << 8) | g->buf[g->len - 1]);
	crc = m17_crc16(g->buf, (uint16_t)(g->len - 2));
	return crc == got ? 1 : -1;
}

#endif
