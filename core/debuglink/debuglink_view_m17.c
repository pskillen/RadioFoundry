// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The m17 section: M17 conventional on the channel (docs/spec/m17.md 8.1, 10): the call heard, the CAN in force,
// whether BERT is running, and the engine's counters. "m17.built=0" without CONFIG_M17
#include "config.h"
#include "debuglink/debuglink.h"
#if CONFIG_M17
#include "radio/radio_m17.h"
#endif

void debuglink_view_m17(const radio_view_t *v, dl_text_t *t)
{
#if CONFIG_M17
	const radio_m17_view_t *m = &v->m17;
	radio_m17_stats_t c;

	dl_text(t, "m17.built=1\nm17.on=%u\nm17.fit=%u\nm17.src=%s\nm17.dst=%s\nm17.can=%u\nm17.late=%u\n",
		(m->flags & RADIO_M17_ON) != 0, m->fit, m->caller, m->dest, m->can, (m->flags & RADIO_M17_LATE) != 0);
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
