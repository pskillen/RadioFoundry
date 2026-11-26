// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 conventional on a channel, the radio core's side (radio_m17.h)
#include "config.h"
#if CONFIG_M17	// the whole file
#include <stddef.h>
#include <string.h>
#include "board.h"
#include "codeplug/codeplug_modes.h"
#include "radio/radio_m17.h"
#include "radio/radio_bands.h"
#include "radio/radio_lend.h"
#include "m17/m17_engine.h"
#include "m17/m17_rx.h"
#include "m17/m17_bert.h"
#include "m17/m17_codeplug.h"
#include "air/air_session.h"

_Static_assert(RADIO_M17_CALL_LEN == M17_CP_CALL_LEN, "M17 callsign buffers must match");

// What one session borrows: the transmit engine, the receiver, the air session (its framer), and the framer's
// hard and soft frame buffers. It has to fit the smaller lend block. Do not shrink the M17 types to make it.
typedef struct {
	m17_engine_t tx;
	m17_rx_t rx;
	air_session_t session;
	uint8_t hard[(M17_FRAME_SYMS + 3) / 4];
	int8_t soft[M17_FRAME_SYMS];
} m17_block_t;

_Static_assert(sizeof(m17_block_t) <= 4096, "M17 engine block does not fit a lend block");

static const radio_m17_settings_t defaults = {
	.can = 0, .can_squelch = false, .dst = M17_ADDR_BROADCAST, .callsign = "",
};
static radio_m17_settings_t set;
static radio_m17_settings_t applied;	// what the running engine was given
static char radio_name[CODEPLUG_RADIO_NAME_LEN + 1];
static m17_block_t *blk;
static bool bert_on;			// a BERT count or transmission was asked for; cleared when the session ends
static radio_m17_stats_t stats;
static air_session_stats_t air_base;
static m17_bert_count_t bert_done;	// sessions already closed; the open one's counters sit on its receiver

void radio_m17_init(void)
{
	radio_m17_end();
	set = defaults;
	applied = defaults;
}

void radio_m17_settings(radio_m17_settings_t *out)
{
	*out = set;
}

void radio_m17_set_radio_name(const char *name)
{
	strncpy(radio_name, name != NULL ? name : "", sizeof radio_name - 1);
	radio_name[sizeof radio_name - 1] = '\0';
}

void radio_m17_set_settings(const radio_m17_settings_t *s)
{
	uint8_t raw[M17_ADDR_BYTES];

	set = *s;
	set.callsign[sizeof set.callsign - 1] = '\0';
	if (set.can > RADIO_M17_CAN_MAX)
		set.can = 0;
	if (set.callsign[0] != '\0' && !m17_addr_encode(set.callsign, raw))
		set.callsign[0] = '\0';
	if (set.dst == 0)
		set.dst = M17_ADDR_BROADCAST;
}

uint32_t radio_m17_fit(void)
{
	return board.air != NULL && board.air->fit != NULL ? board.air->fit(&m17_air_mode.profile) : 0;
}

bool radio_m17_fits(void)
{
	return (radio_m17_fit() & (AIR_FIT_RX | AIR_FIT_TX)) != 0;
}

bool radio_m17_take(channel_t *ch)
{
	if (ch->kind != CODEPLUG_KIND_M17 || !radio_m17_fits() || radio_listen_band(ch->rx_hz))
		return false;
	ch->mode = RADIO_MODE_M17;
	ch->bw = RADIO_BW_12K5;
	return true;
}

bool radio_m17_receives(const channel_t *ch, uint16_t number)
{
	(void)ch;
	(void)number;
	return (radio_m17_fit() & AIR_FIT_RX) != 0;
}

static uint64_t own_addr(const char *call)
{
	uint8_t raw[M17_ADDR_BYTES];
	uint64_t v = 0;

	if (call == NULL || call[0] == '\0' || !m17_addr_encode(call, raw))
		return 0;
	for (uint8_t i = 0; i < M17_ADDR_BYTES; i++)
		v = (v << 8) | raw[i];
	return v;
}

// A temporary M17 mode on a channel without stored M17 settings squelches on its CAN (decision 35).
void radio_m17_base(const channel_t *ch, uint16_t number, radio_m17_settings_t *out)
{
	radio_m17_settings_t s = set;
	m17_chan_t cc;

	if (number != 0 && m17_chan_load(number, ch, &cc)) {
		s.can = cc.can;
		s.can_squelch = cc.can_squelch;
		s.dst = cc.dst;
	} else if (number != 0) {
		s.can_squelch = true;
	}
	*out = s;
}

static void params(const radio_m17_settings_t *base, uint64_t dst, m17_engine_cfg_t *c)
{
	applied = *base;
	if (applied.callsign[0] == '\0')
		m17_callsign_load(radio_name, applied.callsign);
	if (dst != 0)
		applied.dst = dst;
	memset(c, 0, sizeof *c);
	c->own = own_addr(applied.callsign);
	c->dst = applied.dst;
	c->can = applied.can;
	c->rx_can = applied.can;
	c->can_squelch = applied.can_squelch;
	c->tx_kind = M17_TX_VOICE;
	c->silence = NULL;
}

static bool callsign_ok(void)
{
	uint8_t raw[M17_ADDR_BYTES];

	return applied.callsign[0] != '\0' && m17_addr_encode(applied.callsign, raw);
}

// Symbols the engine has built, while the provider's queue has room. A voice transmission keeps producing
// frames until it is stopped; an empty read is the end marker already sent.
static void tx_fill(void *ctx)
{
	m17_block_t *b = ctx;
	uint8_t buf[M17_FRAME_SYMS / 4];

	for (;;) {
		uint16_t space = air_session_tx_space(&b->session);
		uint16_t room, got;

		if (space < 4)
			return;
		room = space / 4;
		if (room > sizeof buf)
			room = (uint16_t)sizeof buf;
		got = m17_engine_tx_read(&b->tx, buf, room);
		if (got == 0) {
			air_session_tx_finish(&b->session);
			return;
		}
		if (!air_session_tx(&b->session, buf, 0, (uint16_t)(got * 4)))
			return;
	}
}

static const air_session_ops_t session_ops = { tx_fill, NULL };

static void count_air(const air_session_stats_t *a)
{
	stats.rx_chunks += a->rx_chunks - air_base.rx_chunks;
	stats.tx_underruns += a->tx_underruns - air_base.tx_underruns;
	stats.tx_syms += a->tx_syms - air_base.tx_syms;
	air_base = *a;
}

static void bert_sum(const m17_bert_count_t *live)
{
	stats.bert_bits = bert_done.bits + (live != NULL ? live->bits : 0);
	stats.bert_errors = bert_done.errors + (live != NULL ? live->errors : 0);
	stats.bert_frames = bert_done.frames + (live != NULL ? live->frames : 0);
	stats.bert_lost = bert_done.lost + (live != NULL ? live->lost : 0);
}

static m17_block_t *place(void)
{
	radio_lend_t blocks[RADIO_LEND_MAX];
	uint8_t n = radio_lend_gather(blocks);

	for (uint8_t i = 0; i < n; i++) {
		if (blocks[i].len >= sizeof(m17_block_t) && ((uintptr_t)blocks[i].p & 7u) == 0)
			return blocks[i].p;
	}
	return NULL;
}

bool radio_m17_begin(const radio_m17_settings_t *base, uint64_t dst)
{
	m17_engine_cfg_t cfg;
	m17_block_t *b;

	radio_m17_end();
	stats.begins++;
	b = place();
	if (b == NULL || (radio_m17_fit() & (AIR_FIT_RX | AIR_FIT_TX)) == 0) {
		stats.begin_fails++;
		return false;
	}
	memset(b, 0, sizeof *b);
	params(base, dst, &cfg);
	m17_engine_init(&b->tx, &cfg, NULL, NULL);
	m17_rx_init(&b->rx, NULL, NULL);
	m17_rx_set_can(&b->rx, cfg.rx_can, cfg.can_squelch);
	if (!air_session_open(&b->session, board.air, &m17_air_mode, &b->rx, b->hard, b->soft, M17_FRAME_SYMS,
			      &session_ops, b)) {
		stats.begin_fails++;
		return false;
	}
	memset(&air_base, 0, sizeof air_base);
	blk = b;
	return true;
}

void radio_m17_end(void)
{
	m17_block_t *b = blk;

	bert_on = false;
	if (b == NULL)
		return;
	// The provider is closed before the pointer goes: nothing it calls can touch the block after this
	count_air(&b->session.stats);
	bert_done.bits += b->rx.bert.bits;
	bert_done.errors += b->rx.bert.errors;
	bert_done.frames += b->rx.bert.frames;
	bert_done.lost += b->rx.bert.lost;
	bert_sum(NULL);
	air_session_close(&b->session);
	blk = NULL;
}

bool radio_m17_running(void)
{
	return blk != NULL;
}

void radio_m17_bert(uint8_t cmd)
{
	if (cmd == 3) {
		bert_on = false;
		if (blk != NULL)
			blk->tx.cfg.tx_kind = M17_TX_VOICE;
		return;
	}
	if (blk == NULL)
		return;
	if (cmd == 1) {
		// Keep what this session already counted, then count from here
		bert_on = true;
		bert_done.bits += blk->rx.bert.bits;
		bert_done.errors += blk->rx.bert.errors;
		bert_done.frames += blk->rx.bert.frames;
		bert_done.lost += blk->rx.bert.lost;
		m17_bert_count_init(&blk->rx.bert);
		if (air_session_tx_state(&blk->session) == AIR_TX_IDLE)
			blk->tx.cfg.tx_kind = M17_TX_VOICE;
		return;
	}
	if (cmd == 2 && air_session_tx_state(&blk->session) == AIR_TX_IDLE) {
		bert_on = true;
		blk->tx.cfg.tx_kind = M17_TX_BERT;
	}
}

bool radio_m17_bert_on(void)
{
	return bert_on;
}

void radio_m17_retuned(void)
{
	if (blk == NULL)
		return;
	m17_rx_reset(&blk->rx);
	air_session_retuned(&blk->session);
}

bool radio_m17_apply(const radio_m17_settings_t *base, uint64_t dst)
{
	m17_engine_cfg_t cfg;

	if (blk == NULL || air_session_tx_state(&blk->session) != AIR_TX_IDLE)
		return false;
	params(base, dst, &cfg);
	cfg.tx_kind = blk->tx.cfg.tx_kind;
	blk->tx.cfg = cfg;
	m17_rx_set_can(&blk->rx, cfg.rx_can, cfg.can_squelch);
	return true;
}

bool radio_m17_in_force(radio_m17_settings_t *out)
{
	if (blk == NULL)
		return false;
	*out = applied;
	return true;
}

static void show_addr(const uint8_t raw[M17_ADDR_BYTES], char *out)
{
	if (!m17_addr_decode(raw, out, RADIO_M17_CALL_LEN))
		out[0] = '\0';
}

static void put_dst(uint64_t dst, char *out)
{
	uint8_t b[M17_ADDR_BYTES];
	uint64_t v = dst == 0 ? M17_ADDR_BROADCAST : dst;

	for (int i = (int)M17_ADDR_BYTES - 1; i >= 0; i--) {
		b[i] = (uint8_t)v;
		v >>= 8;
	}
	show_addr(b, out);
}

static void settings_view(radio_m17_view_t *v)
{
	const radio_m17_settings_t *s = blk != NULL ? &applied : &set;

	v->can = s->can;
	v->fit = (uint16_t)radio_m17_fit();
	put_dst(s->dst, v->tx_dst);
}

bool radio_m17_poll(radio_m17_view_t *v)
{
	radio_m17_view_t n;

	if (blk == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
	} else {
		const m17_rx_t *rx = &blk->rx;

		count_air(&blk->session.stats);
		bert_sum(&rx->bert);
		memset(&n, 0, sizeof n);
		n.flags = RADIO_M17_ON;
		n.tx = (uint8_t)air_session_tx_state(&blk->session);
		if (rx->in_call) {
			n.flags |= RADIO_M17_RX;
			if (rx->late)
				n.flags |= RADIO_M17_LATE;
			if (rx->crc_ok) {
				show_addr(rx->lsf + M17_ADDR_BYTES, n.caller);
				show_addr(rx->lsf, n.dest);
			}
		}
	}
	settings_view(&n);
	bool changed = memcmp(&n, v, sizeof n) != 0;
	*v = n;
	return changed;
}

static bool m17_no_tx(void)
{
	return blk != NULL && (blk->session.fit & AIR_FIT_TX) == 0;
}

static uint8_t prepare_deny;

uint8_t radio_m17_prepare_deny(void)
{
	return prepare_deny;
}

// A voice or packet transmission needs a callsign. BERT does not, and a session with none still runs.
// A missing callsign is named. A failed engine start stays unnamed: there is no separate voice-memory failure yet.
static bool m17_tx_prepare(void)
{
	m17_block_t *b = blk;

	prepare_deny = 0;
	if (b == NULL || !air_session_is_open(&b->session))
		return false;
	if (b->tx.cfg.tx_kind != M17_TX_BERT && !callsign_ok()) {
		prepare_deny = 1;
		return false;
	}
	if (!m17_engine_tx_start(&b->tx, 0, NULL, 0))
		return false;
	return air_session_tx_prime(&b->session);
}

static bool m17_tx_go(void)
{
	return blk != NULL && air_session_tx_go(&blk->session);
}

static void m17_tx_stop(void)
{
	if (blk != NULL)
		m17_engine_tx_stop(&blk->tx);
}

static void m17_tx_abort(void)
{
	if (blk == NULL)
		return;
	m17_engine_tx_stop(&blk->tx);
	air_session_tx_abort(&blk->session);
}

static uint8_t m17_tx_state(void)
{
	return blk != NULL ? (uint8_t)air_session_tx_state(&blk->session) : (uint8_t)AIR_TX_IDLE;
}

static void m17_tx_clear(void)
{
	if (blk != NULL) {
		air_session_tx_clear(&blk->session);
		blk->tx.cfg.tx_kind = M17_TX_VOICE;	// a BERT transmission was selected for this key-up
	}
}

const radio_air_ops_t radio_m17_air = {
	radio_m17_running, m17_no_tx, m17_tx_prepare, m17_tx_go, m17_tx_stop, m17_tx_abort, m17_tx_state,
	m17_tx_clear,
};

void radio_m17_stats(radio_m17_stats_t *out)
{
	*out = stats;
}

void radio_m17_reset_stats(void)
{
	memset(&stats, 0, sizeof stats);
	memset(&bert_done, 0, sizeof bert_done);
	memset(&air_base, 0, sizeof air_base);
}

const void *radio_m17_engine_mem(void)
{
	return blk != NULL ? (const void *)&blk->tx : NULL;
}

size_t radio_m17_engine_bytes(void)
{
	return sizeof(m17_block_t);
}

#endif	// CONFIG_M17
