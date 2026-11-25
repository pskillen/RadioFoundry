// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// M17 in the codeplug (core/m17/m17_codeplug.c, docs/spec/m17-codeplug.md) on a simulated NOR flash: the shared
// vectors (tests/vectors/channel-modes.txt, also read by the Codeplug Editor's tests), channel tags and callsigns
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "codeplug/codeplug.h"
#include "codeplug/codeplug_kv.h"
#include "codeplug/codeplug_tag.h"
#include "m17/m17_codeplug.h"
#include "m17/m17.h"
#include "storage/storage.h"
#include "sim/sim_flash.h"

static int failures;

#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

void storage_lock(void) {}
void storage_unlock(void) {}

static bool fits;
bool radio_m17_fits(void) { return fits; }

#define FLASH_SIZE	0x200000u

static const hal_flash_map_t map = {
	.settings_addr = 0x11000u, .settings_size = 0x8000u, .journal_addr = 0x19000u,
	.openfw_eeprom = 0, .openfw_flash = 0x20000u, .openfw_end = FLASH_SIZE,
	.calibration_addr = 0x10000u, .calibration_size = 0x200u,
};

static band_t bands[] = { { 136000000, 174000000, true }, { 400000000, 900000000, true } };

const board_t board = { .name = "test", .bands = bands, .band_count = 2, .power_count = 9, .flash = &sim_flash };

static void fresh(void)
{
	sim_flash_create(FLASH_SIZE, &map);
	CHECK(storage_init());
	codeplug_init();
}

static int unhex(const char *s, uint8_t *out, int max)
{
	int n = 0;

	if (strcmp(s, "-") == 0)
		return -1;
	while (s[0] != '\0' && s[1] != '\0' && n < max) {
		char byte[3] = { s[0], s[1], '\0' };
		out[n++] = (uint8_t)strtoul(byte, NULL, 16);
		s += 2;
	}
	return n;
}

static uint64_t hex(const char *s) { return strtoull(s, NULL, 16); }

static void test_vectors(void)
{
	FILE *f = fopen("../vectors/channel-modes.txt", "r");
	char line[256];
	int conv = 0, enc = 0, dstbad = 0, rec = 0, call = 0;

	CHECK(f != NULL);
	if (f == NULL)
		return;
	while (fgets(line, sizeof line, f) != NULL) {
		char *rest = NULL;
		uint8_t b[64];

		line[strcspn(line, "\r\n")] = '\0';
		char *kind = strtok_r(line, " ", &rest);
		if (kind == NULL || strncmp(kind, "m17", 3) != 0)
			continue;
		if (strcmp(kind, "m17conv") == 0) {
			int len = unhex(strtok_r(NULL, " ", &rest), b, sizeof b);
			uint64_t can = hex(strtok_r(NULL, " ", &rest));
			bool squelch = hex(strtok_r(NULL, " ", &rest)) != 0;
			uint64_t dst = hex(strtok_r(NULL, " ", &rest));
			m17_chan_t c;

			m17_chan_decode(b, len, &c);
			CHECK(c.can == can && c.can_squelch == squelch && c.dst == dst);
			conv++;
		} else if (strcmp(kind, "m17enc") == 0) {
			m17_chan_t c = { 0 };
			uint8_t encoded[M17_CP_CHAN_LEN];

			c.can = (uint8_t)hex(strtok_r(NULL, " ", &rest));
			c.can_squelch = hex(strtok_r(NULL, " ", &rest)) != 0;
			int len = unhex(strtok_r(NULL, " ", &rest), b, sizeof b);
			CHECK(m17_dst_parse(rest, &c.dst));
			m17_chan_encode(&c, encoded);
			CHECK(len == M17_CP_CHAN_LEN && memcmp(encoded, b, sizeof encoded) == 0);
			enc++;
		} else if (strcmp(kind, "m17dstbad") == 0) {
			uint64_t dst;

			CHECK(!m17_dst_parse(rest, &dst));
			dstbad++;
		} else if (strcmp(kind, "m17rec") == 0 || strcmp(kind, "m17call") == 0) {
			int len = unhex(strtok_r(NULL, " ", &rest), b, sizeof b);
			const char *want = strtok_r(NULL, " ", &rest);
			bool valid = strcmp(want, "-") != 0;
			char out[M17_CP_CALL_LEN];

			if (strcmp(kind, "m17rec") == 0) {
				CHECK(m17_call_decode(b, len, out) == valid);
				rec++;
			} else {
				CHECK(m17_callsign_pick(b, len, rest, out) == valid);
				call++;
			}
			CHECK(strcmp(out, valid ? want : "") == 0);
		}
	}
	fclose(f);
	CHECK(conv >= 4 && enc >= 4 && dstbad >= 4 && rec >= 4 && call >= 4);
}

static void test_dst_edges(void)
{
	uint64_t dst;
	m17_chan_t c = { .can = 16, .can_squelch = false };
	uint8_t b[M17_CP_CHAN_LEN];
	const uint8_t want[M17_CP_CHAN_LEN] = { 0x10 };
	const uint64_t all[] = { M17_ADDR_BROADCAST, 0, M17_CP_ADDR_RESERVED };

	CHECK(!m17_dst_parse(" AB", &dst));
	CHECK(m17_dst_parse("AB1CD  ", &dst) && dst == 0x9FDD51u);
	CHECK(m17_dst_parse("all", &dst) && dst == M17_ADDR_BROADCAST);
	CHECK(m17_dst_parse("", &dst) && dst == M17_ADDR_BROADCAST);
	for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) {
		c.dst = all[i];
		m17_chan_encode(&c, b);
		CHECK(memcmp(b, want, sizeof b) == 0);
	}
}

static void test_chan_load(void)
{
	channel_t ch = {
		.name = "M17 test", .rx_hz = 453100000, .tx_hz = 453100000, .mode = RADIO_MODE_DMR,
		.rx_only = true, .power = CODEPLUG_POWER_DEFAULT, .color_code = 1, .timeslot = 1,
		.system = CODEPLUG_SYSTEM_NONE,
	};
	codeplug_tag_t tag = { .kind = CODEPLUG_KIND_M17, .system = CODEPLUG_SYSTEM_NONE, .flags = CODEPLUG_TAG_TX };
	const uint8_t block[] = { 0x13, 0, 0, 0, 0x0E, 0xD8, 0x7D, 0xAA, 0xBB };
	m17_chan_t c;

	fresh();
	CHECK(!codeplug_kind_built(CODEPLUG_KIND_M17));
	fits = true;
	CHECK(codeplug_kind_built(CODEPLUG_KIND_M17));
	CHECK(codeplug_set_channel(10, &ch));
	CHECK(codeplug_channel(10, &ch) && ch.kind == CODEPLUG_KIND_NONE && !m17_chan_load(10, &ch, &c));
	CHECK(codeplug_tag_write(10, ch.name, &tag, block, M17_CP_CHAN_LEN));
	CHECK(codeplug_channel(10, &ch) && ch.kind == CODEPLUG_KIND_M17);
	CHECK(m17_chan_load(10, &ch, &c) && c.can == 3 && !c.can_squelch && c.dst == 0xED87Du);
	CHECK(codeplug_tag_write(10, ch.name, &tag, NULL, 0));
	CHECK(m17_chan_load(10, &ch, &c) && c.can == 0 && c.can_squelch && c.dst == M17_ADDR_BROADCAST);
	CHECK(codeplug_tag_write(10, ch.name, &tag, block, sizeof block));
	CHECK(m17_chan_load(10, &ch, &c) && c.can == 3 && !c.can_squelch && c.dst == 0xED87Du);
	tag.kind = CODEPLUG_KIND_NXDN;
	CHECK(codeplug_tag_write(10, ch.name, &tag, NULL, 0));
	CHECK(!m17_chan_load(10, &ch, &c));
	CHECK(codeplug_channel(10, &ch) && ch.kind == CODEPLUG_KIND_NXDN && !m17_chan_load(10, &ch, &c));
}

static void test_callsign_load(void)
{
	const uint8_t valid[M17_CP_CALL_BYTES] = "AB1CD";
	const uint8_t invalid[M17_CP_CALL_BYTES] = "AB*CD";
	const uint8_t longer[12] = { 'A', 'B', '1', 'C', 'D', 0, 0, 0, 0, 0, 1, 2 };
	char out[M17_CP_CALL_LEN];

	fresh();
	CHECK(m17_callsign_load("Q0TEST", out) && strcmp(out, "Q0TEST") == 0);
	CHECK(codeplug_kv_set(0x0A06u, valid, sizeof valid));
	CHECK(m17_callsign_load("Q0TEST", out) && strcmp(out, "AB1CD") == 0);
	CHECK(codeplug_kv_set(0x0A06u, invalid, sizeof invalid));
	CHECK(m17_callsign_load("Q0TEST", out) && strcmp(out, "Q0TEST") == 0);
	CHECK(!m17_callsign_load(NULL, out) && out[0] == '\0');
	CHECK(!m17_callsign_load("Bob's", out) && out[0] == '\0');
	CHECK(m17_callsign_load("  q0test home", out) && strcmp(out, "Q0TEST") == 0);
	CHECK(codeplug_kv_set(0x0A06u, longer, sizeof longer));
	CHECK(m17_callsign_load("Q0TEST", out) && strcmp(out, "AB1CD") == 0);
}

static void test_callsign_store(void)
{
	const uint8_t longer[12] = { 'a', 'b', '1', 'c', 'd', 0, 0, 0, 0, 0, 1, 2 };
	const uint8_t shortrec[4] = { 'A', 'B', 0, 0 };
	uint8_t raw[16];
	char out[M17_CP_CALL_LEN];
	int n;

	fresh();
	CHECK(!m17_callsign_stored(out) && out[0] == '\0');
	CHECK(m17_callsign_store("ab1cd"));
	n = codeplug_kv_get(0x0A06u, raw, sizeof raw);
	CHECK(n == 10 && memcmp(raw, "AB1CD", 6) == 0 && raw[9] == 0);
	CHECK(m17_callsign_stored(out) && strcmp(out, "AB1CD") == 0);
	CHECK(!m17_callsign_store("AB*CD"));
	CHECK(m17_callsign_stored(out) && strcmp(out, "AB1CD") == 0);
	CHECK(!m17_callsign_store("ABCDEFGHIJ"));
	CHECK(m17_callsign_stored(out) && strcmp(out, "AB1CD") == 0);
	CHECK(codeplug_kv_set(0x0A06u, longer, sizeof longer));
	CHECK(m17_callsign_store("zz9"));
	n = codeplug_kv_get(0x0A06u, raw, sizeof raw);
	CHECK(n == 12 && raw[0] == 'Z' && raw[1] == 'Z' && raw[2] == '9' && raw[3] == 0);
	CHECK(raw[10] == 1 && raw[11] == 2);
	CHECK(m17_callsign_store(""));
	n = codeplug_kv_get(0x0A06u, raw, sizeof raw);
	CHECK(n == 12 && raw[0] == 0 && raw[9] == 0 && raw[10] == 1 && raw[11] == 2);
	CHECK(!m17_callsign_stored(out));
	CHECK(codeplug_kv_set(0x0A06u, shortrec, sizeof shortrec));
	CHECK(m17_callsign_store("cd"));
	n = codeplug_kv_get(0x0A06u, raw, sizeof raw);
	CHECK(n == 10 && raw[0] == 'C' && raw[1] == 'D' && raw[2] == 0);
}

int main(void)
{
	test_vectors();
	test_dst_edges();
	test_chan_load();
	test_callsign_load();
	test_callsign_store();
	sim_flash_free();
	if (failures) {
		printf("m17CodeplugTest: %d failure(s)\n", failures);
		return 1;
	}
	printf("m17CodeplugTest: all passed\n");
	return 0;
}
