// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "debuglink/debuglink.h"
#include "debuglink/dl_fmt.h"
#include "m17/m17.h"
#include "radio/radio_m17.h"

bool m17_addr_decode(const uint8_t raw[M17_ADDR_BYTES], char *out, size_t cap) { (void)raw; (void)cap; out[0] = '\0'; return false; }

void radio_m17_stats(radio_m17_stats_t *out) { memset(out, 0, sizeof *out); }
bool radio_m17_in_force(radio_m17_settings_t *out) { memset(out, 0, sizeof *out); return false; }
bool radio_m17_bert_on(void) { return false; }
void radio_m17_reset_stats(void) {}

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static const radio_view_t empty;

int main(void)
{
	char buf[256];
	dl_text_t t;

	dl_text_init(&t, buf, sizeof buf);
	debuglink_view_m17(&empty, &t);
	CHECK(strstr(buf, "m17.packet.built=0\n") != NULL);
	printf("# m17MessageDebugTestNoPacket passed\n");
	return 0;
}