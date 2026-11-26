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
_Static_assert(RADIO_M17_META_LEN == M17_META_TEXT_BYTES + 1, "M17 META buffers must match");

// What one session borrows: the transmit engine, the receiver, the air session (its framer), and the framer's
// hard and soft frame buffers. It has to fit the smaller lend block. Do not shrink the M17 types to make it.
typedef struct {
	m17_engine_t tx;
	m17_rx_t rx;
	air_session_t session;
#if CONFIG_M17_PACKET
	radio_m17_msg_decoded_t msg;	// decode workspace used only by the synchronous RX hook
	bool rx_restarting;
	uint32_t packet_started_base, packet_underruns_base;
#endif
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

#if CONFIG_M17_PACKET
static const radio_m17_inbox_ops_t *inbox_ops;
static radio_m17_msg_store_t inbox;
static uint32_t last_received_id;
static radio_m17_packet_stats_t packet_stats;
static m17_packet_rx_stats_t packet_base;	// last protocol counters folded into packet_stats
static uint8_t packet_tx_status;		// radio task only; survives releasing the engine lend
static radio_m17_packet_rx_snap_t packet_rx_snap;
static radio_m17_packet_tx_snap_t packet_tx_snap;

static void m17_tx_abort(void);

void radio_m17_inbox_bind(const radio_m17_inbox_ops_t *ops)
{
	inbox_ops = ops;
}

static void inbox_lock(void)
{
	if (inbox_ops != NULL)
		inbox_ops->lock();
}

static void inbox_unlock(void)
{
	if (inbox_ops != NULL)
		inbox_ops->unlock();
}

void radio_m17_inbox_get(radio_m17_inbox_t *out)
{
	inbox_lock();
	radio_m17_msg_store_summary(&inbox, &out->summary);
	out->last_received_id = last_received_id;
	inbox_unlock();
}

bool radio_m17_inbox_copy(uint32_t id, radio_m17_msg_entry_t *out)
{
	inbox_lock();
	bool found = radio_m17_msg_store_get(&inbox, id, out);
	inbox_unlock();
	return found;
}

bool radio_m17_inbox_copy_at(size_t index, radio_m17_msg_entry_t *out)
{
	inbox_lock();
	bool found = radio_m17_msg_store_get_at(&inbox, index, out);
	inbox_unlock();
	return found;
}

bool radio_m17_inbox_read(uint32_t id)
{
	inbox_lock();
	bool found = radio_m17_msg_store_read(&inbox, id);
	inbox_unlock();
	return found;
}

bool radio_m17_inbox_delete(uint32_t id)
{
	inbox_lock();
	bool found = radio_m17_msg_store_delete(&inbox, id);
	inbox_unlock();
	return found;
}

void radio_m17_packet_stats(radio_m17_packet_stats_t *out)
{
	inbox_lock();
	*out = packet_stats;
	inbox_unlock();
}

void radio_m17_packet_rx_snap(radio_m17_packet_rx_snap_t *out)
{
	if (!out)
		return;
	inbox_lock();
	*out = packet_rx_snap;
	inbox_unlock();
}

void radio_m17_packet_tx_snap(radio_m17_packet_tx_snap_t *out)
{
	if (!out)
		return;
	inbox_lock();
	*out = packet_tx_snap;
	inbox_unlock();
}

void radio_m17_packet_tx_snap_arm(uint64_t serial, uint64_t dst)
{
	inbox_lock();
	packet_tx_snap.tx_seq = serial;
	packet_tx_snap.tx_dst = dst;
	packet_tx_snap.tx_result = 0;
	inbox_unlock();
}

void radio_m17_packet_tx_snap_note(uint64_t serial, uint8_t reason)
{
	if (serial == 0)
		return;
	inbox_lock();
	if (packet_tx_snap.tx_seq == serial)
		packet_tx_snap.tx_result = reason;
	inbox_unlock();
}

// The radio task folds the provider's counters with the scheduler held. The session baseline is
// dropped at each RX reset or close, and observers copy only the accumulated counters.
static void packet_capture(const m17_packet_rx_stats_t *live)
{
	packet_stats.crc_ok += live->crc_ok - packet_base.crc_ok;
	packet_stats.crc_fail += live->crc_fail - packet_base.crc_fail;
	packet_stats.fec_reject += live->fec_reject - packet_base.fec_reject;
	packet_stats.can_reject += live->can_reject - packet_base.can_reject;
	packet_stats.fec_samples += live->fec_samples - packet_base.fec_samples;
	if (live->fec_peak > packet_stats.fec_peak)
		packet_stats.fec_peak = live->fec_peak;
	packet_base = *live;
}

static void rx_packet(const uint8_t *data, uint16_t len, void *ctx)
{
	m17_block_t *b = ctx;
	uint8_t own[M17_ADDR_BYTES];

	// The provider's deferred task owns data and the scratch, even during open. Copy the address
	// under the same gate as apply, then validate the whole packet with the scheduler running.
	inbox_lock();
	if (b->rx_restarting) {
		inbox_unlock();
		return;
	}
	uint64_t addr = b->tx.cfg.own;
	inbox_unlock();
	for (unsigned i = sizeof own; i != 0; i--, addr >>= 8)
		own[i - 1] = (uint8_t)addr;

	memset(&b->msg, 0, sizeof b->msg);
	radio_m17_msg_result_t result = radio_m17_msg_decode(b->rx.lsf, own, data, len, &b->msg);
	uint32_t now = result == RADIO_M17_MSG_ADMITTED && inbox_ops != NULL ? inbox_ops->now_ms() : 0;

	inbox_lock();
	switch (result) {
	case RADIO_M17_MSG_ADMITTED: {
		radio_m17_msg_summary_t summary;

		last_received_id = radio_m17_msg_store_add(&inbox, &b->msg, now);
		radio_m17_msg_store_summary(&inbox, &summary);
		packet_rx_snap.rx_seq = summary.received_seq;
		packet_rx_snap.rx_len = (uint16_t)(len > 0 ? len - 1u : 0u);
		packet_rx_snap.rx_cut = b->msg.cut;
		(void)m17_addr_decode(b->msg.caller, packet_rx_snap.rx_src, sizeof packet_rx_snap.rx_src);
		packet_rx_snap.rx_wire_len = (uint8_t)(packet_rx_snap.rx_len < RADIO_M17_PACKET_DL_HEX_MAX ?
			packet_rx_snap.rx_len : RADIO_M17_PACKET_DL_HEX_MAX);
		memcpy(packet_rx_snap.rx_wire, data, packet_rx_snap.rx_wire_len);
		break;
	}
	case RADIO_M17_MSG_UNSUPPORTED_TYPE:
		packet_stats.unsupported_type++;
		break;
	case RADIO_M17_MSG_ADDRESS:
		packet_stats.bad_address++;
		break;
	case RADIO_M17_MSG_MALFORMED_TEXT:
		packet_stats.malformed_text++;
		break;
	}
	inbox_unlock();
}

static const m17_engine_hooks_t rx_hooks = { .rx_packet = rx_packet };
#endif

void radio_m17_init(void)
{
	radio_m17_end();
#if CONFIG_M17_PACKET
	inbox_lock();
	radio_m17_msg_store_init(&inbox);
	last_received_id = 0;
	packet_tx_status = RADIO_M17_PACKET_TX_NONE;
	memset(&packet_stats, 0, sizeof packet_stats);
	memset(&packet_rx_snap, 0, sizeof packet_rx_snap);
	memset(&packet_tx_snap, 0, sizeof packet_tx_snap);
	inbox_unlock();
#endif
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

void radio_m17_set_vfo_can(bool squelch, uint8_t value)
{
	if (squelch)
		set.can_squelch = value != 0;
	else
		set.can = value;
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

#if CONFIG_M17_PACKET
bool radio_m17_rx_locked(void)
{
	inbox_lock();
	bool locked = blk != NULL && (blk->session.fit & AIR_FIT_RX) && air_framer_locked(&blk->session.fr);
	inbox_unlock();
	return locked;
}

uint64_t radio_m17_packet_tx_source(void)
{
	uint64_t src = blk != NULL ? blk->tx.cfg.own : 0;

	return src < M17_CP_ADDR_RESERVED ? src : 0;
}

static void packet_tx_restore(m17_block_t *b)
{
	b->tx.active = false;
	b->tx.cfg.dst = applied.dst;
	b->tx.cfg.tx_kind = M17_TX_VOICE;
}

bool radio_m17_packet_tx_prepare(uint64_t dst, const uint8_t *data, uint16_t len)
{
	m17_block_t *b = blk;

	if (b == NULL || !air_session_is_open(&b->session) || !(b->session.fit & AIR_FIT_TX) ||
	    air_session_tx_state(&b->session) != AIR_TX_IDLE || b->tx.cfg.tx_kind != M17_TX_VOICE ||
	    radio_m17_packet_tx_source() == 0 || dst == 0 ||
	    (dst >= M17_CP_ADDR_RESERVED && dst != M17_ADDR_BROADCAST) ||
	    data == NULL || len == 0 || len > RADIO_M17_MSG_TEXT_MAX + 1 ||
	    data[len - 1] != 0 || memchr(data, 0, len - 1) != NULL)
		return false;

	b->tx.cfg.dst = dst;
	b->tx.cfg.tx_kind = M17_TX_PACKET;
	if (!m17_engine_tx_start(&b->tx, 0x05, data, len)) {
		packet_tx_restore(b);
		return false;
	}
	b->packet_started_base = b->session.stats.tx_chunks_started;
	b->packet_underruns_base = b->session.stats.tx_underruns;
	packet_tx_status = RADIO_M17_PACKET_TX_PENDING;
	return true;
}

radio_m17_packet_tx_status_t radio_m17_packet_tx_status(void)
{
	m17_block_t *b = blk;
	radio_m17_packet_tx_status_t status = (radio_m17_packet_tx_status_t)packet_tx_status;

	if (b == NULL || status != RADIO_M17_PACKET_TX_PENDING)
		return status;
	inbox_lock();
	if (air_session_tx_state(&b->session) == AIR_TX_DONE) {
		status = b->session.tx_end_reason == AIR_TX_END_DRAINED && b->session.finished &&
			 b->session.stats.tx_chunks_started != b->packet_started_base &&
			 b->session.stats.tx_underruns == b->packet_underruns_base ?
			 RADIO_M17_PACKET_TX_DRAINED : RADIO_M17_PACKET_TX_FAILED;
	}
	inbox_unlock();
	return status;
}

// Before a successful go there is no transmission to wait for. Drop the queue even if go marked DONE
// on refusal, and complete locally because a provider can flush an unstarted queue without an event.
static void packet_tx_cancel_unstarted(m17_block_t *b)
{
	packet_tx_status = RADIO_M17_PACKET_TX_FAILED;
	if (air_session_tx_state(&b->session) != AIR_TX_IDLE) {
		b->session.air->tx_abort();
		b->session.tx = AIR_TX_DONE;
	}
	packet_tx_restore(b);
}

static void packet_tx_fail_active(m17_block_t *b)
{
	packet_tx_status = RADIO_M17_PACKET_TX_FAILED;
	// A deferred SPACE event may precede the abort's DONE. Stop refills before the flush.
	inbox_lock();
	if (air_session_tx_state(&b->session) == AIR_TX_ON)
		b->session.tx = AIR_TX_ENDING;
	inbox_unlock();
}
#endif

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
#if CONFIG_M17_PACKET
	m17_rx_init(&b->rx, &rx_hooks, b);
#else
	m17_rx_init(&b->rx, NULL, NULL);
#endif
	m17_rx_set_can(&b->rx, cfg.rx_can, cfg.can_squelch);
	if (!air_session_open(&b->session, board.air, &m17_air_mode, &b->rx, b->hard, b->soft, M17_FRAME_SYMS,
			      &session_ops, b)) {
		stats.begin_fails++;
#if CONFIG_M17_PACKET
		inbox_lock();
		packet_capture(&b->rx.pkt_stats);
		memset(&packet_base, 0, sizeof packet_base);
		inbox_unlock();
#endif
		return false;
	}
	memset(&air_base, 0, sizeof air_base);
#if CONFIG_M17_PACKET
	inbox_lock();
#endif
	blk = b;
#if CONFIG_M17_PACKET
	inbox_unlock();
#endif
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
#if CONFIG_M17_PACKET
	if (b->tx.cfg.tx_kind == M17_TX_PACKET) {
		if (air_session_tx_state(&b->session) <= AIR_TX_PRIMING)
			packet_tx_cancel_unstarted(b);
		else
			packet_tx_fail_active(b);
	}
#endif
	air_session_close(&b->session);
#if CONFIG_M17_PACKET
	if (b->tx.cfg.tx_kind == M17_TX_PACKET)
		packet_tx_restore(b);
	inbox_lock();
	packet_capture(&b->rx.pkt_stats);
	memset(&packet_base, 0, sizeof packet_base);
#endif
	blk = NULL;
#if CONFIG_M17_PACKET
	inbox_unlock();
#endif
}

bool radio_m17_running(void)
{
	return blk != NULL;
}

void radio_m17_bert(uint8_t cmd)
{
#if CONFIG_M17_PACKET
	if (blk != NULL && blk->tx.cfg.tx_kind == M17_TX_PACKET)
		return;
#endif
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
#if CONFIG_M17_PACKET
	// The provider may deliver RX during restart, which can take its own lock. Suppress inbox
	// admission until the old provenance has been reset, and keep the provider call outside the gate.
	m17_block_t *b = blk;

	if (b->tx.cfg.tx_kind == M17_TX_PACKET)
		m17_tx_abort();
	inbox_lock();
	b->rx_restarting = true;
	inbox_unlock();
	board.air->rx_restart();
	inbox_lock();
	packet_capture(&b->rx.pkt_stats);
	memset(&packet_base, 0, sizeof packet_base);
	m17_rx_reset(&b->rx);
	air_framer_rx_reset(&b->session.fr);
	b->rx_restarting = false;
	inbox_unlock();
#else
	m17_rx_reset(&blk->rx);
	air_session_retuned(&blk->session);
#endif
}

bool radio_m17_apply(const radio_m17_settings_t *base, uint64_t dst)
{
	m17_engine_cfg_t cfg;

	if (blk == NULL || air_session_tx_state(&blk->session) != AIR_TX_IDLE)
		return false;
#if CONFIG_M17_PACKET
	if (blk->tx.cfg.tx_kind == M17_TX_PACKET)
		return false;
#endif
	params(base, dst, &cfg);
	cfg.tx_kind = blk->tx.cfg.tx_kind;
#if CONFIG_M17_PACKET
	inbox_lock();
#endif
	blk->tx.cfg = cfg;
	m17_rx_set_can(&blk->rx, cfg.rx_can, cfg.can_squelch);
#if CONFIG_M17_PACKET
	inbox_unlock();
#endif
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

static radio_m17_rx_state_t rx_state(const m17_rx_t *rx, uint16_t type)
{
	if (!rx->type_known)
		return RADIO_M17_RX_UNKNOWN;
	if (!m17_rx_can_passes(rx))
		return RADIO_M17_RX_OTHER_CAN;
	if (type & M17_TYPE_ENC_MASK)
		return RADIO_M17_RX_ENCRYPTED;
	if (!(type & M17_TYPE_STREAM))
		return RADIO_M17_RX_DATA;
	switch (type & M17_TYPE_DATA_MASK) {
	case M17_TYPE_VOICE_DATA: return RADIO_M17_RX_VOICE_DATA;
	case M17_TYPE_DATA: return RADIO_M17_RX_DATA;
	case M17_TYPE_VOICE: return RADIO_M17_RX_VOICE;
	default: return RADIO_M17_RX_UNKNOWN;
	}
}

// The display takes Latin-1 and complete three-byte UTF-8 glyphs. Convert network UTF-8 within
// each contiguous run, so a missing block can't join bytes from two unrelated glyphs.
static char *meta_run(char *out, const uint8_t *in, unsigned len)
{
	for (unsigned at = 0; at < len;) {
		unsigned start = at;
		uint32_t cp = in[at++];
		unsigned tail = cp < 0xC2u ? 0 : cp < 0xE0u ? 1 : cp < 0xF0u ? 2 : cp <= 0xF4u ? 3 : 0;
		if (tail) {
			cp &= 0x7Fu >> (tail + 1);
			unsigned left = tail;
			while (left && at < len && (in[at] & 0xC0u) == 0x80u) {
				cp = (cp << 6) | (in[at++] & 0x3Fu);
				left--;
			}
			if (left || cp < (tail == 1 ? 0x80u : tail == 2 ? 0x800u : 0x10000u) ||
			    (cp >= 0xD800u && cp <= 0xDFFFu))
				cp = '?';
		} else if (cp >= 0x80u) {
			cp = '?';
		}
		if ((cp >= 0x20u && cp <= 0x7Eu) || (cp >= 0xA0u && cp <= 0xFFu))
			*out++ = (char)cp;
		else if (CONFIG_LANG_ZH && cp >= 0x800u && cp <= 0xFFFFu) {
			memcpy(out, in + start, 3);
			out += 3;
		} else {
			*out++ = '?';
		}
	}
	return out;
}

static void received_meta(const m17_rx_t *rx, uint16_t type, radio_m17_view_t *v)
{
	if (type & M17_TYPE_ENC_MASK)
		return;
	if ((type & M17_TYPE_SUB_MASK) == M17_TYPE_META_CALL) {
		char origin[RADIO_M17_CALL_LEN];
		uint8_t encoded[M17_ADDR_BYTES];
		const uint8_t *raw = rx->lsf + M17_LSF_META_OFFSET;

		show_addr(raw, origin);
		if (origin[0] && strcmp(origin, "ALL") != 0 && m17_addr_encode(origin, encoded) &&
		    memcmp(raw, encoded, sizeof encoded) == 0 &&
		    strcmp(origin, v->caller) != 0) {
			strcpy(v->via, v->caller);
			strcpy(v->caller, origin);
		}
		return;
	}
	char *out = v->meta;
	for (unsigned i = 0; i < 4;) {
		if (!(rx->meta_have & (1u << i))) {
			i++;
			continue;
		}
		unsigned start = i++;
		while (i < 4 && (rx->meta_have & (1u << i)))
			i++;
		if (out != v->meta)
			*out++ = ' ';
		out = meta_run(out, rx->meta_text + start * M17_META_TEXT_BLOCK_BYTES,
			       (i - start) * M17_META_TEXT_BLOCK_BYTES);
	}
	while (out > v->meta && out[-1] == ' ')
		out--;
	*out = '\0';
}

bool radio_m17_poll(radio_m17_view_t *v)
{
	radio_m17_view_t n;

	if (blk == NULL) {
		n = *v;		// the last call heard stays
		n.flags = 0;
		n.tx = AIR_TX_IDLE;
		n.rx_state = RADIO_M17_RX_NONE;
		n.rx_can = 0;
		memset(n.via, 0, sizeof n.via);
		memset(n.meta, 0, sizeof n.meta);
	} else {
		const m17_rx_t *rx = &blk->rx;

		count_air(&blk->session.stats);
		bert_sum(&rx->bert);
#if CONFIG_M17_PACKET
		inbox_lock();
		packet_capture(&rx->pkt_stats);
		inbox_unlock();
#endif
		memset(&n, 0, sizeof n);
		n.flags = RADIO_M17_ON;
		n.tx = (uint8_t)air_session_tx_state(&blk->session);
		if (rx->in_call) {
			n.flags |= RADIO_M17_RX;
			if (rx->late)
				n.flags |= RADIO_M17_LATE;
			if (rx->link) {
				uint16_t type = m17_rx_type(rx);
				n.rx_state = (uint8_t)rx_state(rx, type);
				n.rx_can = rx->type_known ? (type & M17_TYPE_CAN_MASK) >> M17_TYPE_CAN_SHIFT : UINT8_MAX;
				if (rx->crc_ok) {
					show_addr(rx->lsf + M17_ADDR_BYTES, n.caller);
					show_addr(rx->lsf, n.dest);
					received_meta(rx, type, &n);
				}
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
#if CONFIG_M17_PACKET
	if (b->tx.cfg.tx_kind == M17_TX_PACKET) {
		if (air_session_tx_prime(&b->session))
			return true;
		m17_tx_abort();
		return false;
	}
#endif
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
#if CONFIG_M17_PACKET
	if (blk != NULL && blk->tx.cfg.tx_kind == M17_TX_PACKET) {
		air_tx_state_t state = air_session_tx_state(&blk->session);

		if (state != AIR_TX_PRIMING) {
			if (state == AIR_TX_IDLE)
				packet_tx_cancel_unstarted(blk);
			return false;
		}
		if (air_session_tx_go(&blk->session))
			return true;
		packet_tx_cancel_unstarted(blk);
		return false;
	}
#endif
	return blk != NULL && air_session_tx_go(&blk->session);
}

static void m17_tx_stop(void)
{
#if CONFIG_M17_PACKET
	if (blk != NULL && blk->tx.cfg.tx_kind == M17_TX_PACKET)
		return;	// a PTT release must not truncate a packet
#endif
	if (blk != NULL)
		m17_engine_tx_stop(&blk->tx);
}

static void m17_tx_abort(void)
{
	if (blk == NULL)
		return;
#if CONFIG_M17_PACKET
	if (blk->tx.cfg.tx_kind == M17_TX_PACKET) {
		if (air_session_tx_state(&blk->session) <= AIR_TX_PRIMING) {
			packet_tx_cancel_unstarted(blk);
			return;
		}
		packet_tx_fail_active(blk);
	}
#endif
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
#if CONFIG_M17_PACKET
		if (blk->tx.cfg.tx_kind == M17_TX_PACKET) {
			air_tx_state_t state = air_session_tx_state(&blk->session);
			if (state <= AIR_TX_PRIMING) {
				packet_tx_cancel_unstarted(blk);
			} else if (state == AIR_TX_DONE) {
				packet_tx_status = (uint8_t)radio_m17_packet_tx_status();
				packet_tx_restore(blk);
			} else {
				return;
			}
		}
#endif
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
#if CONFIG_M17_PACKET
	inbox_lock();
	memset(&packet_stats, 0, sizeof packet_stats);
	memset(&packet_base, 0, sizeof packet_base);
	if (blk != NULL)
		memset(&blk->rx.pkt_stats, 0, sizeof blk->rx.pkt_stats);
	inbox_unlock();
#endif
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
