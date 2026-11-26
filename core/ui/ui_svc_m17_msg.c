// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Copied M17 inbox snapshots for the UI task, independent of the DMR data service.
#include "config.h"
#if CONFIG_M17 && CONFIG_M17_PACKET
#include <string.h>
#include "ui_svc.h"
#include "radio/radio_m17.h"
#include "hal_time.h"

_Static_assert(UI_MSG_TEXT_MAX == RADIO_M17_MSG_DISPLAY_MAX, "M17 display text capacity");

void ui_svc_m17_msg_summary(ui_m17_msg_summary_t *out)
{
	radio_m17_inbox_t inbox;
	radio_m17_inbox_get(&inbox);
	out->received_seq = inbox.summary.received_seq;
	out->last_received_id = inbox.last_received_id;
	out->count = inbox.summary.count;
	out->unread = inbox.summary.unread;
}

static void message_copy(const radio_m17_msg_entry_t *entry, ui_msg_t *out)
{
	memset(out, 0, sizeof *out);
	out->via = UI_MSG_VIA_M17;
	out->unread = entry->unread;
	out->cut = entry->message.cut;
	out->age_ms = hal_millis() - entry->timestamp;
	m17_addr_decode(entry->message.caller, out->call, sizeof out->call);
	memcpy(out->text, entry->message.text, sizeof out->text);
}

bool ui_svc_m17_msg_get_at(uint16_t i, uint32_t *id, ui_msg_t *out)
{
	radio_m17_msg_entry_t entry;
	if (!radio_m17_inbox_copy_at(i, &entry))
		return false;
	*id = entry.id;
	message_copy(&entry, out);
	return true;
}

bool ui_svc_m17_msg_get(uint32_t id, ui_msg_t *out)
{
	radio_m17_msg_entry_t entry;
	if (!radio_m17_inbox_copy(id, &entry))
		return false;
	message_copy(&entry, out);
	return true;
}

bool ui_svc_m17_msg_read(uint32_t id)
{
	return radio_m17_inbox_read(id);
}

bool ui_svc_m17_msg_delete(uint32_t id)
{
	return radio_m17_inbox_delete(id);
}
#endif
