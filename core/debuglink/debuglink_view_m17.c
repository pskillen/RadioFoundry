// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The m17 section: M17 conventional on the channel (docs/spec/m17.md 8.1, 10): the call heard, the CAN in force,
// whether BERT is running, and the engine's counters. "m17.built=0" without CONFIG_M17
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_M17
#include "m17/m17.h"
#include "radio/radio_m17.h"
#endif

void debuglink_view_m17(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_M17
	const radio_m17_view_t *m = &v->m17;
	radio_m17_stats_t c;
	radio_m17_settings_t s;

	dl_text(t, "m17.built=1\nm17.on=%u\nm17.fit=%u\nm17.src=%s\nm17.dst=%s\nm17.can=%u\nm17.late=%u\n",
		(m->flags & RADIO_M17_ON) != 0, m->fit, m->caller, m->dest, m->can, (m->flags & RADIO_M17_LATE) != 0);
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
