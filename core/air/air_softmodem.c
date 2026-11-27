// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The soft modem's receiver (air_softmodem.h, docs/spec/air-interface.md 6).
//
// Input samples -> minus the tap's offset -> polyphase interpolator to five samples per symbol -> integrate and
// dump -> the line (a ring of the last 28 symbols of filtered samples). On every filtered sample the newest symbol
// spaced samples in the line can be fitted to each sync pattern (sample = offset + level x pattern); where the fit's
// error passes through a minimum, that sample is a symbol center, and the fit's level and offset are the signal's.
// Symbols leave from the far end of the line, so a sync is sampled on the timing it gave.
//
// The cost is per filtered sample (24,000 a second at 4800 symbols), so that path is kept short (../dsp/fxp.h has
// the rules and the kernels): the fit runs where the step and sign gates allow a pattern or a candidate needs it, the
// input history is laid out twice so the interpolator reads one run, and nothing in it divides or needs 64 bits.
#include "air_softmodem.h"
#include "air_rx_work.h"

#include <string.h>

#include "../dsp/fxp.h"
#include "air_syms.h"

#define SPS		AIR_SOFTMODEM_SPS
#define LINE		AIR_SOFTMODEM_LINE
#define TAPS		AIR_SOFTMODEM_TAPS
#define PHASES		12			// interpolator phases per input sample
#define LINE_LVL	16			// line units per symbol level at nominal: +3 is 48, headroom to 127
#define EMIT_AT		(LINE - 2 * SPS)	// a symbol leaves when its center is this far back
#define Q_NONE		0xFFFFu
// A fit's error: 256 x the mean square distance of the samples from the fitted line, in symbol levels. 92 is 0.6
// of a level RMS, where slicing already fails often: a sync worse than that gives no timing worth taking
#define Q_MAX		92
#define LVL_NOM		(LINE_LVL * 256)
#define LVL_MAX		(LVL_NOM * 3 / 2)
#define LVL_MIN_KNOWN	(LVL_NOM * 7 / 12)	// as the framer's limits
#define LVL_MIN_TRACKED	(LVL_NOM / 4)		// the tracked level sits low after noise: the fit corrects it
#define OFF_MAX		(LINE_LVL * 3 / 2 * 256)	// half the outer level
#define TRACK_SHIFT_MIN	4
#define TRACK_SHIFT_MAX	10
#define HOLE_US		2000			// input time off by more: samples were lost
#define GAIN_MAX	(1u << 20)		// 16 line units per summed count

uint32_t air_softmodem_search_calls;
// The gate (filtered, below): the steps of four levels or more between a pattern's neighboring symbols, as bits.
// Require all but one of the pattern's large steps in the newest 17 symbols. Short all-outer patterns
// with few steps use an exact sign history instead.

// The interpolator: a windowed sinc low-pass at 0.45 of the input rate, 12 phases x 8 taps, each phase summing to
// 16384. Row p, tap t weighs the input sample t back from the newest; the output sits (47.5 - p) / 12 samples back
static const int16_t interp[PHASES][TAPS] = {
	{      0,    105,   -630,   1791,  14707,    735,   -394,     70 },
	{     -1,    143,   -884,   3021,  14382,   -130,   -189,     42 },
	{     -2,    182,  -1141,   4393,  13753,   -797,    -24,     20 },
	{     -4,    216,  -1376,   5867,  12843,  -1268,    101,      5 },
	{     -7,    240,  -1565,   7393,  11699,  -1557,    185,     -4 },
	{     -8,    248,  -1678,   8914,  10369,  -1685,    232,     -8 },
	{     -8,    232,  -1685,  10369,   8914,  -1678,    248,     -8 },
	{     -4,    185,  -1557,  11699,   7393,  -1565,    240,     -7 },
	{      5,    101,  -1268,  12843,   5867,  -1376,    216,     -4 },
	{     20,    -24,   -797,  13753,   4393,  -1141,    182,     -2 },
	{     42,   -189,   -130,  14382,   3021,   -884,    143,     -1 },
	{     70,   -394,    735,  14707,   1791,   -630,    105,      0 },
};

static uint32_t gcd32(uint32_t a, uint32_t b)
{
	while (b) {
		uint32_t t = a % b;

		a = b;
		b = t;
	}
	return a;
}

// The longest run between two syncs the profile sends, any length (0: none recurs)
static uint16_t sync_spacing(const air_profile_t *p)
{
	return p->sync_any_syms ? p->sync_any_syms : p->sync_every_syms;
}

static bool rate_ok(const air_profile_t *p, uint32_t rate_hz, uint16_t *step, uint16_t *den)
{
	uint32_t out = (uint32_t)p->sym_rate * SPS, g;

	// Below 8 kHz a 4800 symbol/s signal does not fit; above twice the filtered rate the interpolator (which does
	// not filter for the lower rate) would fold noise in
	if (!p->sym_rate || rate_hz < 8000 || rate_hz > 2 * out || rate_hz < (uint32_t)p->sym_rate * 5 / 3)
		return false;
	g = gcd32(rate_hz, out);
	if (rate_hz / g + out / g > 0xFFFF)
		return false;
	*step = (uint16_t)(rate_hz / g);
	*den = (uint16_t)(out / g);
	return true;
}

uint32_t air_softmodem_rx_fit(const air_profile_t *p, const hal_baseband_t *bb)
{
	uint16_t step, den;

	if (!bb || !bb->rx_start || !bb->rx_read || bb->rx_channels != 1)
		return 0;		// a split band (two channels) is not written
	if (p->levels != 2 && p->levels != 4)
		return 0;
	if (!rate_ok(p, bb->rx_rate_hz, &step, &den))
		return 0;
	// The band: up to a little past half the symbol rate, and down to where a run of one level still holds (four
	// levels have no room for a sagging baseline)
	if (bb->rx_hi_hz < (uint32_t)p->sym_rate * 11 / 20)
		return 0;
	if (bb->rx_lo_hz > p->sym_rate / (p->levels == 4 ? 100 : 20))
		return 0;
	return AIR_FIT_RX | AIR_FIT_SOFT;
}

void air_softmodem_rx_reset(air_softmodem_t *m)
{
	memset(m->in, 0, sizeof m->in);
	memset(m->sum, 0, sizeof m->sum);
	m->acc = 0;
	m->in_at = m->sum_at = 0;
	m->pos = 0;
	m->line_at = m->line_n = 0;
	memset(m->steps, 0, sizeof m->steps);
	memset(m->short_signs, 0, sizeof m->short_signs);
	m->phase = 0;
	m->track_shift = TRACK_SHIFT_MIN;
	m->track_n = 1u << TRACK_SHIFT_MIN;
	m->locked = false;
	m->running = false;
	m->cand_q = m->cand_prev_q = Q_NONE;
	m->hold = 0;
	m->due = EMIT_AT - 1;
	m->sub = 0;
	m->scale = (uint16_t)((AIR_SOFT_OUTER / 3) * 256 / LINE_LVL);
	m->off = 0;
	m->since_sync = 0xFFFF;
	m->sync_wait = 0;
	m->gap = true;
	m->n = 0;
	air_softmodem_search_calls = 0;
}

bool air_softmodem_rx_open(air_softmodem_t *m, const air_profile_t *p, uint32_t rate_hz, uint16_t counts_per_khz,
			   air_softmodem_out_t out, void *ctx)
{
	uint16_t step, den;

	if ((p->levels != 2 && p->levels != 4) || !rate_ok(p, rate_hz, &step, &den))
		return false;
	memset(m, 0, sizeof *m);
	m->p = p;
	m->out = out;
	m->ctx = ctx;
	m->rate_hz = rate_hz;
	m->step = step;
	m->den = den;
	// The integrate and dump over a symbol is the receive filter the C4FM pulse is made for, and close to a matched
	// one for a 2-level square pulse. A root raised cosine or Gaussian pulse gets a shorter one: it only smooths
	m->box = (p->shape == AIR_SHAPE_C4FM || p->shape == AIR_SHAPE_NRZ) ? SPS : 3;
	m->gain_known = counts_per_khz != 0;
	if (m->gain_known) {
		// The outer symbol (dev_outer_hz) to 3 x LINE_LVL, after the sum over box samples
		uint64_t outer = (uint64_t)p->dev_outer_hz * counts_per_khz;	// counts x 1000

		if (!outer)
			return false;
		outer = ((uint64_t)(3 * LINE_LVL) * 65536u * 1000u) / (outer * m->box);
		m->gain = outer > GAIN_MAX ? GAIN_MAX : (uint32_t)outer;
	} else {
		m->gain = 65536u / m->box;
		m->env = m->env_fast = 3 * LINE_LVL * 1024;
	}
	for (uint8_t i = 0; i < p->nsyncs && i < AIR_SYNCS_MAX; i++) {
		const air_sync_t *s = &p->sync[i];
		int16_t s1 = 0, s2 = 0;
		unsigned large_steps = 0;
		uint32_t positive = 0;

		if (s->nsyms < 8 || s->nsyms > AIR_SYNC_SYMS_MAX)
			continue;
		for (uint8_t k = 0; k < s->nsyms; k++) {
			int8_t e = air_sym_level(air_sym_get(s->syms, k));

			s1 = (int16_t)(s1 + e);
			s2 = (int16_t)(s2 + e * e);
			positive = (positive << 1) | (uint32_t)(e > 0);
		}
		if (s->nsyms * s2 - s1 * s1 <= 0)
			continue;	// one level only: nothing to fit
		m->valid |= (uint8_t)(1u << i);
		// The gate's bits: bit j is the step from the symbol j + 1 back to the symbol j back
		for (unsigned j = 0; j + 1 < s->nsyms && j < 16; j++) {
			int d = air_sym_level(air_sym_get(s->syms, (uint16_t)(s->nsyms - 1 - j)))
				- air_sym_level(air_sym_get(s->syms, (uint16_t)(s->nsyms - 2 - j)));

			if (d >= 4)
				m->gate_up[i] |= (uint16_t)(1u << j);
			if (d >= 4 || d <= -4) {
				m->gate_mask[i] |= (uint16_t)(1u << j);
				large_steps++;
			}
		}
		if (large_steps < 4 && s2 == 9 * s->nsyms) {
			// With exact signs, opposite-sign neighbors already give the correct step direction.
			if (s->nsyms == 8) {
				m->short_gate |= (uint8_t)(1u << i);
				m->short_hash |= 1u << (positive & 31u);
				m->gate_up[i] = (uint16_t)(((positive ^ 0xFFu) << 8) | positive);
			} else {
				m->sign_gate |= (uint8_t)(1u << i);
				m->gate_up[i] = (uint16_t)positive;
				m->gate_mask[i] = (uint16_t)(positive >> 16);
			}
		}
		// For a long all-outer step-gated word, choose disjoint equal-level neighbors. Store their
		// positions in the bits the direction check ignores.
		if (!(m->sign_gate & (1u << i)) && s2 == 9 * s->nsyms && s->nsyms >= 17) {
			for (unsigned j = 0; j < 16; j++) {
				if (!(m->gate_mask[i] & (1u << j))) {
					m->gate_up[i] |= (uint16_t)(1u << j);
					j++;
				}
			}
		}
	}
	air_softmodem_rx_reset(m);
	return true;
}

static int line_back(const air_softmodem_t *m, unsigned back)
{
	int at = (int)m->line_at - 1 - (int)back;

	return m->line[at < 0 ? at + LINE : at];
}

static unsigned short_sign(int x)
{
	return x > 0 ? 1u : x < 0 ? 0x100u : 0;
}

// The sign gate uses the current offset, including for the samples that were already in the line.
static void rebuild_short_signs(air_softmodem_t *m)
{
	unsigned ph = m->phase;

	for (unsigned back = 0; back < SPS; back++) {
		unsigned signs = 0;

		for (unsigned k = 0; k < 8; k++)
			signs |= short_sign(line_back(m, back + k * SPS) * 256 - m->off) << k;
		ph = ph ? ph - 1 : SPS - 1;
		m->short_signs[ph] = (uint16_t)signs;
	}
}

static void flush(air_softmodem_t *m)
{
	uint8_t hard[AIR_SOFTMODEM_CHUNK / 4];
	air_chunk_t c;

	if (!m->n)
		return;
	memset(hard, 0, sizeof hard);
	for (unsigned i = 0; i < m->n; i++) {
		int v = m->soft[i];

		air_sym_put(hard, (uint16_t)i, m->p->levels == 2 ? (v >= 0 ? 1 : 3)
			    : v >= 2 * AIR_SOFT_OUTER / 3 ? 1 : v >= 0 ? 0 : v >= -2 * AIR_SOFT_OUTER / 3 ? 2 : 3);
	}
	c.t_us = m->t_chunk_us;
	c.nsyms = m->n;
	c.sync_at = m->sync_at;
	c.sync_id = m->sync_id;
	c.flags = m->flags;
	c.rssi_dbm = 0;
	c.hard = hard;
	c.soft = m->soft;
	m->n = 0;
	m->out(&c, m->ctx);
}

// The symbol whose center is due samples back leaves the line. t_us: the time of the input sample in hand, less the
// filters' delay
static void emit(air_softmodem_t *m, uint32_t t_us, unsigned pos)
{
	int32_t x = line_back(m, m->due) * 16, v;
	bool sync;

	// Between two samples: straight line to the neighbor on the side the fit's minimum leaned to
	if (m->sub > 0)
		x += m->sub * (line_back(m, m->due - 1u) - line_back(m, m->due));
	else if (m->sub < 0)
		x -= m->sub * (line_back(m, m->due + 1u) - line_back(m, m->due));
	v = ((x - m->off / 16) * (int32_t)m->scale) / 4096;
	if (v > 127)
		v = 127;
	if (v < -127)
		v = -127;
	sync = m->sync_wait && --m->sync_wait == 0;
	if (sync && m->n && m->sync_at >= 0)
		flush(m);		// a chunk names one sync
	if (!m->n) {
		uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;

		// The line's newest sample sits pos / den of an input sample after t_us (an input sample in 1/16 us)
		m->t_chunk_us = t_us + pos * (16000000u / m->rate_hz) / ((uint32_t)m->den * 16u)
				- (uint32_t)(((uint32_t)m->due * 16 - m->sub) * 62500u / out_rate);
		m->flags = m->gap ? AIR_CHUNK_GAP : 0;
		// Past the profile's sync spacing with none seen (or none ever): the timing is the last one's, or none.
		// (since_sync starts when a sync is found, two sync lengths before its symbols are all out)
		if (m->since_sync > (uint32_t)sync_spacing(m->p) + 2 * AIR_SYNC_SYMS_MAX)
			m->flags |= AIR_CHUNK_FREE_RUN;
		m->gap = false;
		m->sync_at = -1;
	}
	if (sync) {
		m->sync_at = m->n;
		m->sync_id = m->sync_wait_id;
	}
	m->soft[m->n] = (int8_t)v;
	m->n++;
	if (m->since_sync != 0xFFFF)
		m->since_sync++;
	if (m->locked) {
		unsigned spacing = sync_spacing(m->p);

		if (m->since_sync > (spacing ? 2 * spacing : 4800u) + 2 * AIR_SYNC_SYMS_MAX)
			m->locked = false;	// the syncs stopped coming: the averages follow the input again
	}
	if (m->n == AIR_SOFTMODEM_CHUNK)
		flush(m);
}

// A sync's last symbol center is `back` samples back: take its timing, level and offset
static void commit(air_softmodem_t *m, uint8_t back, uint16_t q_next)
{
	const air_sync_t *s = &m->p->sync[m->cand_sync];
	int32_t lvl = m->cand_lvl, off = m->cand_off;
	uint8_t first = (uint8_t)(back + (s->nsyms - 1) * SPS), d;
	int8_t sub = 0;

	// The level far from nominal (a tap of unknown or wrong scale): bring the line and the gain to it, so the
	// samples keep their resolution and their headroom
	if (lvl < LVL_NOM * 4 / 5 || lvl > LVL_NOM * 5 / 4) {
		int32_t f = (LVL_NOM * 256) / lvl;	// x 256

		for (unsigned i = 0; i < LINE; i++)
			m->line[i] = (int8_t)fxp_clamp((m->line[i] * f) / 256, -127, 127);
		m->gain = (m->gain * (uint32_t)f) >> 8;		// (f is 1024 at most: 32 bits hold it)
		if (m->gain > GAIN_MAX)
			m->gain = GAIN_MAX;
		m->env = (m->env / f) * 256 + ((m->env % f) * 256) / f;
		m->env_fast = (m->env_fast / f) * 256 + ((m->env_fast % f) * 256) / f;
		off = (off * f) / 256;
		lvl = LVL_NOM;
	}
	m->scale = (uint16_t)(((AIR_SOFT_OUTER / 3) * 65536) / lvl);
	m->off = (int16_t)off;
	if (m->short_gate)
		rebuild_short_signs(m);
	// Where between the samples the error was least: a parabola through the candidate and its two neighbors
	if (m->cand_prev_q != Q_NONE && q_next != Q_NONE) {
		int32_t a = m->cand_prev_q, b = m->cand_q, c = q_next, curv = a - 2 * b + c;

		if (curv > 0) {
			int32_t x = (8 * (a - c)) / curv;

			sub = (int8_t)(x > 8 ? 8 : x < -8 ? -8 : x);
		}
	}
	// The next symbol to leave moves to the nearest center of the new timing: no symbol is lost or sent twice,
	// whatever the old timing was. It is still ahead of the sync's first symbol (EMIT_AT is past a sync's length)
	d = (uint8_t)((m->due + 5 * SPS - first % SPS) % SPS);
	if (d || sub != m->sub)
		flush(m);		// the symbols before are on the old timing: the next chunk's time is the new one
	m->sub = sub;
	m->due = (uint8_t)(d <= SPS / 2 ? m->due - d : m->due + SPS - d);
	m->sync_wait = (uint8_t)((m->due - first) / SPS + 1);
	m->sync_wait_id = s->id;
	m->since_sync = 0;
	m->locked = true;
	m->cand_q = Q_NONE;
	m->hold = SPS;
}

static bool fit_pattern(int32_t n, int32_t sxe, int32_t s1, int32_t det, int32_t num, int32_t sx, int32_t sxx,
			int32_t lvl_min, uint16_t *best, int16_t *lvl, int16_t *off)
{
	int32_t a, b, r, a2;
	uint32_t q;

	if (det <= 0 || num * 256 < lvl_min * det)
		return false;
	a = (num * 256) / det;
	b = (sx * 256 - a * s1) / n;
	if (a > LVL_MAX || b > OFF_MAX || b < -OFF_MAX)
		return false;
	r = sxx - (a * sxe + b * sx) / 256;
	if (r < 0)
		r = 0;
	a2 = ((a * a) >> 16) * n;
	q = ((uint32_t)r << 8) / (uint32_t)a2;
	if (q >= Q_NONE)
		q = Q_NONE - 1;
	if (q >= *best)
		return false;
	*best = (uint16_t)q;
	*lvl = (int16_t)a;
	*off = (int16_t)b;
	return true;
}

// Return the least error among the requested patterns (above Q_MAX too, for the sides of a peak).
static uint16_t search(const air_softmodem_t *m, unsigned back, unsigned mask, uint8_t *which,
		       int16_t *lvl, int16_t *off)
{
	int32_t lvl_min = m->gain_known ? LVL_MIN_KNOWN : LVL_MIN_TRACKED;
	uint16_t best = Q_NONE;

	air_softmodem_search_calls++;
	AIR_RX_ADD(search, 1);
	while (mask) {
		unsigned i = (unsigned)__builtin_ctz(mask);
		const air_sync_t *s = &m->p->sync[i];
		int32_t n = s->nsyms, sx = 0, sxx = 0, sxe = 0, s1 = 0, s2 = 0;
		int at = (int)m->line_at - 1 - (int)back - (n - 1) * SPS;

		mask &= mask - 1;
		if (at < 0)
			at += LINE;
		int k = 0;

		// Four samples share a packed pattern byte and normally one contiguous run of the delay line.
		for (; k + 4 <= n; k += 4) {
			unsigned word = s->syms[k >> 2];
			int e0 = air_sym_level((uint8_t)(word >> 6)), e1 = air_sym_level((uint8_t)(word >> 4));
			int e2 = air_sym_level((uint8_t)(word >> 2)), e3 = air_sym_level((uint8_t)word);
			int x0 = m->line[at], x1, x2, x3;

			if (at + 3 * SPS < LINE) {
				x1 = m->line[at + SPS];
				x2 = m->line[at + 2 * SPS];
				x3 = m->line[at + 3 * SPS];
				at += 4 * SPS;
				if (at >= LINE)
					at -= LINE;
			} else {
				int a1 = at + SPS, a2 = at + 2 * SPS, a3 = at + 3 * SPS;

				x1 = m->line[a1 < LINE ? a1 : a1 - LINE];
				x2 = m->line[a2 < LINE ? a2 : a2 - LINE];
				x3 = m->line[a3 < LINE ? a3 : a3 - LINE];
				at += 4 * SPS - LINE;
			}
			sx += x0 + x1 + x2 + x3;
			sxx += x0 * x0 + x1 * x1 + x2 * x2 + x3 * x3;
			sxe += x0 * e0 + x1 * e1 + x2 * e2 + x3 * e3;
			s1 += e0 + e1 + e2 + e3;
			s2 += e0 * e0 + e1 * e1 + e2 * e2 + e3 * e3;
		}
		for (; k < n; k++) {
			int x = m->line[at];
			int e = air_sym_level(air_sym_get(s->syms, (uint16_t)k));

			sx += x;
			sxx += x * x;
			sxe += x * e;
			s1 += e;
			s2 += e * e;
			at += SPS;
			if (at >= LINE)
				at -= LINE;
		}
		AIR_RX_ADD(fits, 1);
		if (fit_pattern(n, sxe, s1, n * s2 - s1 * s1, n * sxe - s1 * sx,
				sx, sxx, lvl_min, &best, lvl, off))
			*which = (uint8_t)i;
	}
	return best;
}

// Locked profiles with a fixed period: fit only near the next expected sync
static bool search_window(const air_softmodem_t *m)
{
	return !m->locked || m->since_sync + 2 >= m->p->sync_period_syms;
}

// Each disjoint equal-level pair contributes at least d*d/2 to the ideal least-squares residual. A fit
// that starts a peak has r <= (Q_MAX + 1) * (LVL_MAX/256)^2 * n / 256. Truncating the fitted slope can only
// raise r. Truncating the offset can lower it by at most n/2, plus one for rounding the dot product.
// Together this bounds twice the ideal residual by 422*n. The peak's sides still use the full fit.
static bool equal_pairs_fit(const air_softmodem_t *m, unsigned pairs, unsigned n)
{
	unsigned sum = 0;

	while (pairs) {
		unsigned j = (unsigned)__builtin_ctz(pairs);
		int d = line_back(m, j * SPS) - line_back(m, (j + 1) * SPS);

		sum += (unsigned)(d * d);
		if (sum >= 422 * n)
			return false;
		pairs &= pairs - 1;
	}
	return true;
}

// Can a pattern end at the newest sample: allow one missing step, and check signs when there are too few steps.
static unsigned gate(const air_softmodem_t *m, unsigned steps, unsigned signs)
{
	uint32_t positive = 0, zero = 0;
	unsigned gathered = 0, mask_out = 0;
	int at = (int)m->line_at - 1;

	unsigned short_gate = m->short_gate;

	unsigned short_hits = (m->short_hash >> (signs & 31u)) & 1u ? short_gate : 0;

	for (unsigned pending = short_hits; pending; pending &= pending - 1) {
		unsigned i = (unsigned)__builtin_ctz(pending);

		if (signs == m->gate_up[i])
			mask_out |= 1u << i;
	}
	for (unsigned pending = m->valid & ~short_gate; pending; pending &= pending - 1) {
		unsigned i = (unsigned)__builtin_ctz(pending);
		uint32_t expected = m->gate_up[i];
		unsigned mask = m->gate_mask[i];
		unsigned n = 0, v;

		if (m->sign_gate & (1u << i)) {
			n = m->p->sync[i].nsyms;
			expected |= (uint32_t)mask << 16;
			// A sign change is a large step. Its direction is the sign of the newer symbol.
			mask = (uint16_t)((expected ^ (expected >> 1)) & ((1u << (n - 1)) - 1u));
		}
		v = (steps ^ expected) & mask;
		if (v & (v - 1))
			continue;
		if (!n) {
			unsigned pairs = expected & ~mask;

			if (!mask || !pairs || equal_pairs_fit(m, pairs, m->p->sync[i].nsyms))
				mask_out |= 1u << i;
			continue;
		}
		// Gather each symbol-spaced sample only once, even when several patterns need it.
		for (; gathered < n; gathered++) {
			int x;

			if (at < 0)
				at += LINE;
			x = m->line[at] * 256 - m->off;
			positive |= (uint32_t)(x > 0) << gathered;
			zero |= (uint32_t)(x == 0) << gathered;
			at -= SPS;
		}
		if ((((positive ^ expected) | zero) & ((1u << n) - 1u)) == 0)
			mask_out |= 1u << i;
	}
	return mask_out;
}

// Only entered when the gate admitted a pattern or a candidate's peak is being tracked.
static void __attribute__((noinline)) fit_sync(air_softmodem_t *m, unsigned mask)
{
	uint8_t which = 0;
	int16_t lvl = 0, off = 0;
	uint16_t q = search(m, 0, mask, &which, &lvl, &off);

	if (m->cand_q != Q_NONE) {
		if (q >= m->cand_q) {
			commit(m, 1, q);
			return;
		}
		m->cand_prev_q = m->cand_q;
	} else {
		if (q > Q_MAX)
			return;
		uint8_t prev;
		int16_t prev_lvl, prev_off;

		// The gate opens a candidate. Its neighbors still use all patterns to place the timing minimum.
		m->cand_prev_q = search(m, 1, m->valid, &prev, &prev_lvl, &prev_off);
	}
	m->cand_q = q;
	m->cand_sync = which;
	m->cand_lvl = lvl;
	m->cand_off = off;
}

// One filtered sample v (summed input counts, before the gain) into the line; t_us as emit's
static void filtered(air_softmodem_t *m, int32_t v, uint32_t t_us, unsigned pos)
{
	int32_t y = fxp_clamp(fxp_scale16(v, m->gain), -127, 127);
	unsigned at = m->line_at, ph = m->phase, signs = 0;
	unsigned steps = ((unsigned)m->steps[ph] << 1 | (unsigned)(y > m->line[at >= SPS ? at - SPS : at + LINE - SPS])) & 0xFFFFu;

	if (m->short_gate) {
		signs = ((unsigned)m->short_signs[ph] << 1 & 0xFEFEu) | short_sign(y * 256 - m->off);
		m->short_signs[ph] = (uint16_t)signs;
	}
	m->line[at] = (int8_t)y;
	m->line_at = (uint8_t)(at + 1 == LINE ? 0 : at + 1);
	m->steps[ph] = (uint16_t)steps;
	m->phase = (uint8_t)(ph + 1 == SPS ? 0 : ph + 1);
	if (m->line_n < LINE) {
		m->line_n++;
		return;
	}
	m->due++;

	if (m->hold) {
		m->hold--;
	} else if (search_window(m)) {
		unsigned mask = m->cand_q != Q_NONE ? m->valid : gate(m, steps, signs);

		if (mask)
			fit_sync(m, mask);
	} else {
		m->cand_q = Q_NONE;	// a peak started near the period must not coast through the frame middle
	}
	if (m->due >= EMIT_AT) {
		emit(m, t_us, pos);
		m->due = (uint8_t)(m->due - SPS);
	}
}

void air_softmodem_rx(air_softmodem_t *m, const int16_t *samples, uint16_t n, uint32_t t_us)
{
	uint32_t us_q = 1000000u / m->rate_hz, us_r = 1000000u % m->rate_hz, us_acc = 0;
	uint32_t out_rate = (uint32_t)m->p->sym_rate * SPS;
	// From the newest input sample back to the newest filtered one: the interpolator's 47.5 / 12 input samples
	// (less the phase, below) and half the integrate and dump
	uint32_t delay_us = (uint32_t)((47500000ull / 12) / m->rate_hz) + (uint32_t)(m->box - 1) * 500000u / out_rate;
	int32_t target = (m->p->levels == 4 ? 2 : 3) * LINE_LVL;
	unsigned box = m->box, den = m->den;
	// What a filtered sample's step adds to the interpolator phase: whole phases, and the rest in 1/den
	unsigned ph_step = (unsigned)m->step * PHASES / den, ph_rstep = (unsigned)m->step * PHASES % den;

	if (m->running) {
		int32_t d = (int32_t)(t_us - m->t_next_us);

		if (d > HOLE_US || d < -HOLE_US)
			air_softmodem_rx_reset(m);
	}
	m->running = true;
	unsigned pos = m->pos, step = m->step;
	unsigned ph = pos * PHASES / den, ph_r = pos * PHASES % den;

	for (unsigned i = 0; i < n; i++) {
		int32_t x = samples[i], v = x - (m->dc >> 10);
		unsigned at = (m->in_at + 1u) & (TAPS - 1u);

		if (!m->locked) {
			m->dc += (x * 1024 - m->dc) >> m->track_shift;
			if (!m->gain_known) {
				// A random 4-level signal averages 2 levels in magnitude, a 2-level one its only level, 3
				int32_t mag = v < 0 ? -v : v;

				m->env += (mag * 1024 - m->env) >> m->track_shift;
				m->env_fast += (mag * 1024 - m->env_fast) >> TRACK_SHIFT_MIN;
				if (m->env < 1024)
					m->env = 1024;
				// The level stepped by half or double (a signal after a quiet carrier, or the other way): the
				// slow averages would take hundreds of ms to follow, the gain clipping or starving every symbol
				// meanwhile. They start over at their fastest
				if (m->env_fast > 2 * m->env || 2 * m->env_fast < m->env) {
					m->env = m->env_fast < 1024 ? 1024 : m->env_fast;
					m->track_shift = TRACK_SHIFT_MIN;
					m->track_n = 1u << TRACK_SHIFT_MIN;
				}
				m->gain = (((uint32_t)target << 26) / (uint32_t)m->env) / box;
			}
			if (m->track_shift < TRACK_SHIFT_MAX && --m->track_n == 0) {
				m->track_shift++;
				m->track_n = (uint16_t)(1u << m->track_shift);
			}
		} else if (!m->gain_known) {
			// The lock's level is held, but a fast average still watches the magnitude. A chance sync in
			// discriminator noise must not keep that level when the carrier comes: the line clips or
			// starves, and a long sync whose gate needs real steps (an M17 link setup) is never fitted.
			int32_t mag = v < 0 ? -v : v;

			m->env_fast += (mag * 1024 - m->env_fast) >> TRACK_SHIFT_MIN;
			if (m->env_fast > 2 * m->env || 2 * m->env_fast < m->env) {
				m->env = m->env_fast < 1024 ? 1024 : m->env_fast;
				m->track_shift = TRACK_SHIFT_MIN;
				m->track_n = 1u << TRACK_SHIFT_MIN;
				m->locked = false;
				m->gain = (((uint32_t)target << 26) / (uint32_t)m->env) / box;
			}
		}
		// Twice, TAPS apart: the last TAPS samples are in[at + 1 ..], oldest first, whatever at is
		m->in[at] = m->in[at + TAPS] = fxp_sat16(v);
		m->in_at = (uint8_t)at;
		while (pos < den) {
			// (a row read backward is the mirror phase's row: the table is symmetric)
			int32_t y = fxp_sat16(fxp_dot16_8(interp[PHASES - 1 - ph], &m->in[at + 1]) >> 14);
			unsigned sa = m->sum_at;

			AIR_RX_ADD(filter_macs, TAPS);
			m->acc += y - m->sum[sa];
			m->sum[sa] = (int16_t)y;
			m->sum_at = (uint8_t)(sa + 1 == box ? 0 : sa + 1);
			filtered(m, m->acc, t_us - delay_us, pos);
			pos += step;
			ph += ph_step;
			ph_r += ph_rstep;
			if (ph_r >= den) {
				ph_r -= den;
				ph++;
			}
		}
		pos -= den;
		ph -= PHASES;
		t_us += us_q;
		us_acc += us_r;
		if (us_acc >= m->rate_hz) {
			us_acc -= m->rate_hz;
			t_us++;
		}
	}
	m->pos = (uint16_t)pos;
	m->t_next_us = t_us;
	flush(m);
}
