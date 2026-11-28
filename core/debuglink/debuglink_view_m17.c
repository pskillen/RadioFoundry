// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The m17 section: M17 conventional on the channel (docs/spec/m17.md 8.1, 10): the call heard, the CAN in force,
// whether BERT is running, and the engine's counters. "m17.built=0" without CONFIG_M17
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_M17
#include "m17/m17.h"
#include "radio/radio_m17.h"
#if CONFIG_DEBUGLINK && CONFIG_M17_VOICE
#include "debuglink/debuglink_m17_bench.h"
#endif
#if CONFIG_M17_VOICE
#include "m17/m17Voice.h"
#endif
#endif

#if CONFIG_M17 && CONFIG_M17_PACKET
static void packet_line(dl_text_t *t, const radio_m17_packet_stats_t *s, const radio_m17_packet_rx_snap_t *rx,
		const radio_m17_packet_tx_snap_t *tx)
{
	uint8_t raw[M17_ADDR_BYTES];
	char dst[RADIO_M17_CALL_LEN];

	dl_text(t, "m17.packet.built=1\nm17.packets=%u\nm17.packet_crc_fail=%u\nm17.fec_bits=%u\n",
		(unsigned)s->crc_ok, (unsigned)s->crc_fail, (unsigned)s->fec_samples);
	dl_text(t, "m17.fec_reject=%u\nm17.can_reject=%u\nm17.packet.unsupported=%u\nm17.packet.bad_address=%u\n"
		"m17.packet.malformed=%u\nm17.packet.fec_peak=%u\n", (unsigned)s->fec_reject, (unsigned)s->can_reject,
		(unsigned)s->unsupported_type, (unsigned)s->bad_address, (unsigned)s->malformed_text,
		(unsigned)s->fec_peak);
	dl_text(t, "m17.packet.rx_seq=%u\nm17.packet.rx_src=%s\nm17.packet.rx_len=%u\nm17.packet.rx_cut=%u\n",
		(unsigned)rx->rx_seq, rx->rx_src, (unsigned)rx->rx_len, rx->rx_cut ? 1u : 0u);
	dl_text(t, "m17.packet.rx_hex=");
	for (uint8_t i = 0; i < rx->rx_wire_len; i++)
		dl_text(t, "%02x", rx->rx_wire[i]);
	dl_text(t, "\n");
	for (unsigned i = 0; i < sizeof raw; i++)
		raw[i] = (uint8_t)(tx->tx_dst >> (8u * (sizeof raw - 1u - i)));
	if (!m17_addr_decode(raw, dst, sizeof dst))
		dst[0] = '\0';
	dl_text(t, "m17.packet.tx_seq=%u\nm17.packet.tx_dst=%s\nm17.packet.tx_result=%u\n",
		(unsigned)tx->tx_seq, dst, (unsigned)tx->tx_result);
}
#endif

void debuglink_view_m17(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_M17
	const radio_m17_view_t *m = &v->m17;
	radio_m17_stats_t c;
	radio_m17_settings_t s;

	dl_text(t, "m17.built=1\nm17.on=%u\nm17.fit=%u\nm17.src=%s\nm17.dst=%s\nm17.can=%u\nm17.late=%u\n",
		(m->flags & RADIO_M17_ON) != 0, m->fit, m->caller, m->dest, m->can, (m->flags & RADIO_M17_LATE) != 0);
#if CONFIG_M17_VOICE
	{
		m17_voice_counts_t vc;

		m17VoiceGetCounts(&vc);
		dl_text(t, "m17.voice_mem=%u\nm17.engine_bytes=%u\n",
			(m->flags & RADIO_M17_VOICE_MEM) != 0 ? 1u : 0u, (unsigned)radio_m17_engine_bytes());
		dl_text(t, "m17v.rx_frames=%u\nm17v.rx_lost=%u\nm17v.rx_underruns=%u\n",
			(unsigned)vc.rx_frames, (unsigned)vc.rx_lost, (unsigned)vc.rx_underruns);
	}
#endif
	if (radio_m17_in_force(&s)) {
		uint8_t raw[M17_ADDR_BYTES];
		char dst[RADIO_M17_CALL_LEN];

		for (unsigned i = 0; i < sizeof raw; i++)
			raw[i] = (uint8_t)(s.dst >> (8u * (sizeof raw - 1u - i)));
		if (!m17_addr_decode(raw, dst, sizeof dst))
			dst[0] = '\0';
		dl_text(t, "m17.run.can=%u\nm17.run.squelch=%u\nm17.run.dst=%s\nm17.run.call=%s\n",
			s.can, s.can_squelch, dst, s.callsign);
	}
	radio_m17_stats(&c);
	dl_text(t, "m17.bert.on=%u\nm17.bert.bits=%u\nm17.bert.errs=%u\nm17.bert.frames=%u\n", radio_m17_bert_on(),
		(unsigned)c.bert_bits, (unsigned)c.bert_errors, (unsigned)c.bert_frames);
	dl_text(t, "m17.begins=%u\nm17.begin_fails=%u\n", (unsigned)c.begins, (unsigned)c.begin_fails);
#if CONFIG_M17_PACKET
	{
		radio_m17_packet_stats_t ps;
		radio_m17_packet_rx_snap_t rx;
		radio_m17_packet_tx_snap_t tx;

		radio_m17_packet_stats(&ps);
		radio_m17_packet_rx_snap(&rx);
		radio_m17_packet_tx_snap(&tx);
		packet_line(t, &ps, &rx, &tx);
	}
#else
	dl_text(t, "m17.packet.built=0\n");
#endif
#if CONFIG_DEBUGLINK && CONFIG_M17_VOICE
	debuglink_m17_bench_view(t);
#endif
#else
	(void)v;
	dl_text(t, "m17.built=0\n");
#endif
}

void debuglink_m17_reset(void)
{
#if CONFIG_M17
	radio_m17_reset_stats();
#endif
}
