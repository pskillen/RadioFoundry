// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "air/air_rx_work.h"
#include "air/air_softmodem.h"
#include "air/air_syms.h"
#include "m17/m17_rx.h"

enum { RATE = 8000, HALF = 256, HALF_US = 32000, FRAME_BYTES = M17_FRAME_SYMS / 4 };
static const uint8_t message[] = "FM copy half bench";

#define CHECK(c) do { \
	if (!(c)) { \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

air_rx_work_t air_rx_work;

typedef struct {
	air_softmodem_t modem;
	air_framer_t framer;
	m17_rx_t rx;
	uint8_t hard[FRAME_BYTES];
	int8_t soft[M17_FRAME_SYMS];
	unsigned delivered;
} receiver_t;

static void packet(const uint8_t *data, uint16_t len, void *ctx)
{
	receiver_t *r = ctx;

	CHECK(len == sizeof message + 1 && data[0] == 0x05);
	CHECK(memcmp(data + 1, message, sizeof message) == 0);
	r->delivered++;
}

static void chunk(const air_chunk_t *c, void *ctx)
{
	air_framer_rx(ctx, c);
}

static void begin(receiver_t *r)
{
	static const m17_engine_hooks_t hooks = { .rx_packet = packet };

	memset(r, 0, sizeof *r);
	m17_rx_init(&r->rx, &hooks, r);
	air_framer_init(&r->framer, &m17_air_mode, &r->rx, r->hard, r->soft, M17_FRAME_SYMS);
	CHECK(air_softmodem_rx_open(&r->modem, &m17_air_mode.profile, RATE, 0, chunk, &r->framer));
}

static double uniform(uint32_t *seed)
{
	*seed = *seed * 1664525u + 1013904223u;
	return (*seed >> 8) * (1.0 / 16777216.0);
}

static void noise(int16_t *samples, uint32_t *seed)
{
	for (unsigned i = 0; i < HALF; i++) {
		double u = uniform(seed);
		double v = uniform(seed);
		double x = 4000 * sqrt(-2 * log(u > 0 ? u : 1e-12)) * cos(2 * M_PI * v);

		samples[i] = (int16_t)lround(x);
	}
}

static double pulse(double x)
{
	if (fabs(x) < 1e-9)
		return 0.5 + 2 / M_PI;
	if (fabs(fabs(x) - 0.5) < 1e-9)
		return (1 + 2 / M_PI) / (2 * sqrt(2));
	return (sin(M_PI * x / 2) + 2 * x * cos(1.5 * M_PI * x)) / (M_PI * x * (1 - 4 * x * x));
}

static void signal(int16_t *samples, const uint8_t *air, unsigned nsyms, unsigned at, unsigned deviation)
{
	for (unsigned i = 0; i < HALF; i++) {
		double t = (at + i) * (4800.0 / RATE);
		int mid = (int)t;
		double v = 0;

		for (int k = mid - 8; k <= mid + 8; k++)
			if (k >= 0 && k < (int)nsyms)
				v += air_sym_level(air_sym_get(air, (uint16_t)k)) * pulse(t - k);
		samples[i] = (int16_t)lround(v * (1600.0 * deviation / 2400));
	}
}

// Each peaks column is the maximum of that work counter across the group of halves.
#define WORK_FIELDS(F) \
	F(search) F(fits) F(filter_macs) F(sync_tries) F(sync_fits) F(heads) F(viterbi) F(viterbi_bits) F(replay_syms)

typedef struct {
	air_rx_work_t peaks, total;
	uint64_t ns_peak, ns_total;
	unsigned halves;
} stats_t;

static uint64_t now_ns(void)
{
	struct timespec t;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
	return (uint64_t)t.tv_sec * 1000000000 + (uint64_t)t.tv_nsec;
}

static void run_half(receiver_t *r, const int16_t *samples, unsigned half, stats_t *s)
{
	memset(&air_rx_work, 0, sizeof air_rx_work);
	uint64_t start = now_ns();

	air_softmodem_rx(&r->modem, samples, HALF, 1000000 + half * HALF_US);
	uint64_t elapsed = now_ns() - start;

	s->ns_total += elapsed;
	if (elapsed > s->ns_peak)
		s->ns_peak = elapsed;
	s->halves++;
	CHECK(air_rx_work.filter_macs == HALF * 3 * AIR_SOFTMODEM_TAPS);
#define ACCUM(f) \
	s->total.f += air_rx_work.f; \
	if (air_rx_work.f > s->peaks.f) \
		s->peaks.f = air_rx_work.f;
	WORK_FIELDS(ACCUM)
#undef ACCUM
}

static void show(const char *label, const stats_t *s)
{
	printf("%s peak", label);
#define PEAK(f) printf(" " #f "=%u", s->peaks.f);
	WORK_FIELDS(PEAK)
#undef PEAK
	printf(" ns=%llu\n", (unsigned long long)s->ns_peak);
	printf("%s mean", label);
#define MEAN(f) printf(" " #f "=%.2f", (double)s->total.f / s->halves);
	WORK_FIELDS(MEAN)
#undef MEAN
	printf(" ns=%.0f\n", (double)s->ns_total / s->halves);
}

static void bench_noise(void)
{
	receiver_t r;
	stats_t s = {0};
	int16_t samples[HALF];
	uint32_t seed = 1;

	begin(&r);
	for (unsigned h = 0; h < 256; h++) {
		noise(samples, &seed);
		run_half(&r, samples, h, &s);
	}
	CHECK(r.delivered == 0);
	show("noise", &s);
	CHECK(s.peaks.search <= 90 && s.peaks.fits <= 100 && s.total.fits <= 50 * s.halves);
	CHECK(s.peaks.sync_tries <= 280 && s.total.sync_tries <= 110 * s.halves);
	CHECK(s.peaks.sync_fits <= 90 && s.peaks.heads <= 7 && s.peaks.viterbi == 0 && s.peaks.replay_syms <= 1344);
}

static void bench_packet(unsigned deviation, bool gap)
{
	enum { LEAD = 4, SIGNAL = 6 };
	m17_engine_cfg_t cfg = { .own = M17_ADDR_AB1CD, .dst = M17_ADDR_BROADCAST, .can = 5, .tx_kind = M17_TX_PACKET };
	m17_engine_t tx;
	uint8_t air[4 * FRAME_BYTES];
	int16_t samples[HALF];
	receiver_t r;
	stats_t lead = {0}, s = {0};
	uint32_t seed = 1;

	m17_engine_init(&tx, &cfg, NULL, NULL);
	CHECK(m17_engine_tx_start(&tx, 0x05, message, sizeof message));
	CHECK(m17_engine_tx_read(&tx, air, sizeof air) == sizeof air);
	begin(&r);
	for (unsigned h = 0; h < LEAD; h++) {
		noise(samples, &seed);
		run_half(&r, samples, h, &lead);
	}
	for (unsigned h = gap ? 1 : 0; h < SIGNAL; h++) {
		signal(samples, air, sizeof air * 4, h * HALF, deviation);
		run_half(&r, samples, LEAD + h, &s);
	}
	CHECK(r.delivered == 1);
	if (gap)
		return;
	char label[24];

	snprintf(label, sizeof label, "text %u Hz", deviation);
	show(label, &s);
	CHECK(s.peaks.search <= 305 && s.peaks.fits <= 305);
	CHECK(s.peaks.sync_tries <= 780 && s.peaks.sync_fits <= 96);
	CHECK(s.peaks.heads <= 1 && s.peaks.viterbi <= 1 && s.peaks.viterbi_bits <= 244 && s.peaks.replay_syms <= 192);
}

static void test_tail_masks(void)
{
	const air_profile_t *p = &m17_air_mode.profile;

	for (unsigned signs = 0; signs < 256; signs++) {
		unsigned candidates = 0;

		for (unsigned i = 0; i < p->nsyncs; i++) {
			const air_sync_t *s = &p->sync[i];
			unsigned errors = 0;

			for (unsigned j = 0; j < 8; j++) {
				unsigned d = air_sym_get(s->syms, (uint16_t)(s->nsyms - 8 + j));

				errors += (d >> 1) != ((signs >> (7 - j)) & 1u);
			}
			if (errors <= s->max_errs)
				candidates |= 1u << i;
		}
		CHECK(candidates == m17_air_mode.sync_tail_masks[signs]);
	}
}

int main(void)
{
	test_tail_masks();
	bench_noise();
	bench_packet(2190, false);
	bench_packet(2540, false);
	// Drop the first RF copy half. The remaining preamble still contains the long link-setup sync.
	bench_packet(2190, true);
	bench_packet(2540, true);
	puts("# PASSED airRxWorkTest");
	return EXIT_SUCCESS;
}
