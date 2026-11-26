// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 codeplug codec (docs/spec/m17-codeplug.md)
#include "config.h"

#if CONFIG_M17

#include <string.h>
#include "m17/m17_codeplug.h"
#include "m17/m17.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "codeplug/codeplug_records.h"
#include "codeplug/codeplug_modes.h"

static uint64_t addr_read(const uint8_t b[M17_ADDR_BYTES])
{
	uint64_t addr = 0;

	for (unsigned i = 0; i < M17_ADDR_BYTES; i++)
		addr = (addr << 8) | b[i];
	return addr;
}

void m17_chan_decode(const uint8_t *b, int len, m17_chan_t *out)
{
	uint8_t flags = len >= 1 ? b[0] : 0;
	uint64_t dst = len >= (int)M17_CP_CHAN_LEN ? addr_read(b + 1) : 0;

	out->can = flags & M17_CP_CAN_MAX;
	out->can_squelch = (flags & M17_CP_SQ_OFF) == 0;
	out->dst = dst == 0 || dst >= M17_CP_ADDR_RESERVED ? M17_ADDR_BROADCAST : dst;
}

void m17_chan_encode(const m17_chan_t *c, uint8_t out[M17_CP_CHAN_LEN])
{
	uint64_t dst = c->dst >= M17_CP_ADDR_RESERVED ? 0 : c->dst;

	out[0] = c->can <= M17_CP_CAN_MAX ? c->can : 0;
	if (!c->can_squelch)
		out[0] |= M17_CP_SQ_OFF;
	for (unsigned i = M17_CP_CHAN_LEN - 1; i > 0; i--) {
		out[i] = (uint8_t)dst;
		dst >>= 8;
	}
}

// Fold just the callsign alphabet so validation is independent of locale.
static char call_char(uint8_t c)
{
	if (c >= 'a' && c <= 'z')
		c = (uint8_t)(c - 'a' + 'A');
	if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '/' || c == '.')
		return (char)c;
	return '\0';
}

bool m17_dst_parse(const char *text, uint64_t *dst)
{
	char clean[M17_ADDR_CHARS_MAX + 1];
	uint8_t addr[M17_ADDR_BYTES];
	size_t n = strlen(text);

	while (n > 0 && text[n - 1] == ' ')
		n--;
	if (n > M17_ADDR_CHARS_MAX || (n > 0 && text[0] == ' '))
		return false;
	for (size_t i = 0; i < n; i++) {
		char c = text[i];

		if (c != ' ' && !(i == 0 && c == '@'))
			c = call_char((uint8_t)c);
		if (c == '\0')
			return false;
		clean[i] = c;
	}
	clean[n] = '\0';
	if (n == 0 || strcmp(clean, "ALL") == 0 || strcmp(clean, "@ALL") == 0) {
		*dst = M17_ADDR_BROADCAST;
		return true;
	}
	if (!m17_addr_encode(clean, addr))
		return false;
	*dst = addr_read(addr);
	return true;
}

static bool call_copy(const uint8_t *b, size_t n, char out[M17_CP_CALL_LEN])
{
	out[0] = '\0';
	if (n == 0 || n >= M17_CP_CALL_LEN)
		return false;
	for (size_t i = 0; i < n; i++) {
		out[i] = call_char(b[i]);
		if (out[i] == '\0') {
			out[0] = '\0';
			return false;
		}
	}
	out[n] = '\0';
	return true;
}

bool m17_call_decode(const uint8_t *b, int len, char out[M17_CP_CALL_LEN])
{
	int n = 0;

	if (len > (int)M17_CP_CALL_BYTES)
		len = (int)M17_CP_CALL_BYTES;
	while (n < len && b[n] != 0 && b[n] != 0xFFu)
		n++;
	while (n > 0 && b[n - 1] == ' ')
		n--;
	return call_copy(b, (size_t)n, out);
}

bool m17_callsign_pick(const uint8_t *b, int len, const char *radio_name, char out[M17_CP_CALL_LEN])
{
	if (m17_call_decode(b, len, out))
		return true;
	if (radio_name == NULL)
		return false;
	while (*radio_name == ' ')
		radio_name++;

	return call_copy((const uint8_t *)radio_name, strcspn(radio_name, " -"), out);
}

bool m17_chan_load(uint16_t number, const channel_t *ch, m17_chan_t *out)
{
	uint8_t b[M17_CP_CHAN_LEN + CODEPLUG_TAG_HEAD_MAX];
	const int cap = M17_CP_CHAN_LEN;

	if (ch->kind != CODEPLUG_KIND_M17)
		return false;
	int len = codeplug_tag_params(number, ch, b, (uint16_t)cap);
	if (len < 0)
		return false;
	m17_chan_decode(b, len < cap ? len : cap, out);
	return true;
}

bool m17_callsign_load(const char *radio_name, char out[M17_CP_CALL_LEN])
{
	uint8_t b[M17_CP_CALL_BYTES];
	const int cap = (int)sizeof b;
	int len = codeplug_mode_settings(CODEPLUG_KIND_M17, b, sizeof b);

	return m17_callsign_pick(b, len < cap ? len : cap, radio_name, out);
}

bool m17_callsign_stored(char out[M17_CP_CALL_LEN])
{
	uint8_t b[M17_CP_CALL_BYTES];
	const int cap = (int)sizeof b;
	int len = codeplug_mode_settings(CODEPLUG_KIND_M17, b, sizeof b);

	if (len < 0)
		len = 0;
	return m17_call_decode(b, len < cap ? len : cap, out);
}

bool m17_callsign_store(const char *text)
{
	uint8_t prefix[M17_CP_CALL_BYTES];
	size_t n = 0;

	memset(prefix, 0, sizeof prefix);
	if (text == NULL)
		text = "";
	while (text[n] != '\0') {
		char c;

		if (n >= M17_ADDR_CHARS_MAX)
			return false;
		c = call_char((uint8_t)text[n]);
		if (c == '\0')
			return false;
		prefix[n++] = (uint8_t)c;
	}
	// The first 10 bytes only: a later field appended to the record stays (codeplug_kv.h, CODEPLUG_KV_HEAD)
	codeplug_kv_op_t op = {
		(uint16_t)(CODEPLUG_KEY_MODE_SET + CODEPLUG_KIND_M17),
		(uint16_t)(M17_CP_CALL_BYTES | CODEPLUG_KV_HEAD), prefix,
	};
	return codeplug_kv_write(&op, 1);
}

bool m17_chan_store(uint16_t number, const channel_t *ch, const m17_chan_t *m)
{
	channel_t c = *ch;
	uint8_t blk[M17_CP_CHAN_LEN];
	uint8_t old[M17_CP_CHAN_LEN + CODEPLUG_TAG_HEAD_MAX];
	codeplug_tag_t t;

	c.kind = CODEPLUG_KIND_M17;
	c.system = 0xFFu;
	c.mode = RADIO_MODE_DMR;
	c.bw = RADIO_BW_12K5;
	c.rx_tone.type = TONE_NONE;
	c.tx_tone.type = TONE_NONE;
	m17_chan_encode(m, blk);
	if (!codeplug_set_channel(number, &c))
		return false;
	// set_channel followed a live tag onto this name, or there is none yet. Rewrite keeps bits 5-7 of the flag
	// byte and anything stored after the 7-byte block. A rewrite that fails is the whole save failing.
	if (codeplug_tag_read(number, &t) && t.kind == CODEPLUG_KIND_M17 &&
	    t.check == codeplug_name_check(c.name)) {
		if (codeplug_tag_params(number, &c, old, M17_CP_CHAN_LEN) >= 1)
			blk[0] = (uint8_t)((blk[0] & 0x1Fu) | (old[0] & 0xE0u));
		t.system = 0xFFu;
		t.flags = (uint8_t)((t.flags & (uint8_t)~CODEPLUG_TAG_TX) | (ch->rx_only ? 0u : CODEPLUG_TAG_TX));
		return codeplug_tag_rewrite(number, c.name, &t, blk, sizeof blk);
	}
	t = (codeplug_tag_t){
		.kind = CODEPLUG_KIND_M17, .system = 0xFFu, .flags = ch->rx_only ? 0u : CODEPLUG_TAG_TX,
	};
	return codeplug_tag_write(number, c.name, &t, blk, sizeof blk);
}

#endif	// CONFIG_M17
