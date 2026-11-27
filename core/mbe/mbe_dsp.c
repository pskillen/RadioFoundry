// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Fixed-point helpers of the MBE core (mbe_dsp.h): 2^x, log2, interpolated cosine, and a real FFT built on a radix-2
// complex FFT with Q15 twiddles from the quarter-wave table.
#include "mbe_dsp.h"
#include "../dsp/fxp.h"

int32_t mbe_cos_i(uint32_t ph)
{
	uint32_t i = ph >> 22;
	int32_t c0 = mbe_sin_idx(i + 256u), c1 = mbe_sin_idx(i + 257u);

	// one step of the quarter wave is at most 201 (32767 pi / 512), so the product stays under 2^30: no 64-bit one
	return c0 + (((c1 - c0) * (int32_t)(ph & 0x3FFFFFu)) >> 22);
}

int32_t mbe_exp2_q11(int32_t x)
{
	int32_t ip = x >> 11;				// floor
	uint32_t fr = (uint32_t)x & 2047u;		// 0..2047
	uint32_t i = fr >> 5, f = fr & 31u;
	uint32_t m = mbe_exp2_tab[i] + (((uint32_t)(mbe_exp2_tab[i + 1] - mbe_exp2_tab[i]) * f) >> 5);	// Q14, 1..2

	if (ip >= 30)
		return 1 << 30;
	if (ip >= 14)
		return (int32_t)(m << (ip - 14));
	if (ip < -1)
		return 0;
	return (int32_t)(m >> (14 - ip));
}

static int clz32(uint32_t x) { return x ? __builtin_clz(x) : 32; }

int32_t mbe_log2_q11(uint32_t x)
{
	int e;
	uint32_t m, fr, i, f, l;

	if (!x)
		return -(64 << 11);
	e = 31 - clz32(x);				// x in [2^e, 2^(e+1))
	m = e >= 16 ? x >> (e - 16) : x << (16 - e);	// Q16, 1..2
	fr = m & 0xFFFFu;
	i = fr >> 10;
	f = fr & 1023u;
	l = mbe_log2_tab[i] + (((uint32_t)(mbe_log2_tab[i + 1] - mbe_log2_tab[i]) * f) >> 10);	// Q15
	return (e << 11) + (int32_t)(l >> 4);
}

int32_t mbe_log2_q11_64(uint64_t x)
{
	uint32_t h = (uint32_t)(x >> 32), l = (uint32_t)x;

	if (h) {
		int s = 32 - clz32(h);			// 1..32: x >> s fits 32 bits, no variable 64-bit shift
		return mbe_log2_q11(s == 32 ? h : (h << (32 - s)) | (l >> s)) + (s << 11);
	}
	return mbe_log2_q11(l);
}

// b is a Q15 twiddle from the quarter wave, a 16-bit factor: fxp_mulshr16's two 32-bit products give fxp_mulshr's result
static inline __attribute__((always_inline)) int32_t mulq15(int32_t a, int32_t b) { return fxp_mulshr16(a, b, 15); }

// FFT sums wrap at 32 bits, including the sums before the forward pass's right shifts.
MBE_INLINE int32_t add32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
MBE_INLINE int32_t sub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }

MBE_INLINE void butterfly(int32_t *a, int32_t *b, int32_t tr, int32_t ti)
{
	int32_t ar = a[0], ai = a[1];

	b[0] = sub32(ar, tr);
	b[1] = sub32(ai, ti);
	a[0] = add32(ar, tr);
	a[1] = add32(ai, ti);
}

// Keep the twiddle and the two data cursors live across the butterflies that share them.
static __attribute__((noinline)) void twiddled_butterflies(int32_t *a, int len, int32_t c, int32_t s)
{
	int32_t *b = a + len;
	int remaining = MBE_FFT_N;

	for (;;) {
		int32_t br = b[0], bi = b[1];
		int32_t tr = sub32(mulq15(br, c), mulq15(bi, s));
		int32_t ti = add32(mulq15(br, s), mulq15(bi, c));

		butterfly(a, b, tr, ti);
		remaining -= len;
		if (!remaining)
			break;
		a += 2 * len;
		b += 2 * len;
	}
}

// Complex FFT of N points, radix 2, decimation in time, no scaling; the inverse (e^{+i}) when inv.
static void cfft(int32_t *z, int inv)
{
	static const uint8_t reverse4[] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
	int32_t *end = z + 2 * MBE_FFT_N;

	_Static_assert(MBE_FFT_LOG == 8, "bit reversal uses two nibbles");
	for (int i = 1; i < MBE_FFT_N; i++) {
		int j = (reverse4[i & 15] << 4) | reverse4[i >> 4];

		if (i < j) {
			int32_t *a = z + 2 * i, *b = z + 2 * j;
			int32_t ar = a[0], ai = a[1];

			a[0] = b[0];
			a[1] = b[1];
			b[0] = ar;
			b[1] = ai;
		}
	}
	// Finish the first two stages in each block of four while the intermediate sums are in registers.
	for (int32_t *p = z; p != end; p += 8) {
		int32_t ar = add32(p[0], p[2]), ai = add32(p[1], p[3]);
		int32_t br = sub32(p[0], p[2]), bi = sub32(p[1], p[3]);
		int32_t cr = add32(p[4], p[6]), ci = add32(p[5], p[7]);
		int32_t dr = sub32(p[4], p[6]), di = sub32(p[5], p[7]);
		int32_t tr = inv ? sub32(0, di) : di, ti = inv ? dr : sub32(0, dr);

		p[0] = add32(ar, cr);
		p[1] = add32(ai, ci);
		p[4] = sub32(ar, cr);
		p[5] = sub32(ai, ci);
		p[2] = add32(br, tr);
		p[3] = add32(bi, ti);
		p[6] = sub32(br, tr);
		p[7] = sub32(bi, ti);
	}
	for (int len = 8, blocks = MBE_FFT_N / 8; blocks; len <<= 1, blocks >>= 1) {
		int half = len >> 1;

		// The k = 0 and k = half/2 butterflies need no Q15 products.
		for (int32_t *p = z; p != end; p += 2 * len) {
			int32_t *a = p + half, *b = a + len;
			int32_t tr = inv ? sub32(0, b[1]) : b[1], ti = inv ? b[0] : sub32(0, b[0]);

			butterfly(p, p + len, p[len], p[len + 1]);
			butterfly(a, b, tr, ti);
		}
		for (int k = 1; k < half; k++) {
			int angle = k * (4 * blocks);
			int32_t c, s;

			if (angle == 256)
				continue;
			// The twiddles span only two quadrants. Reflect the second angle once for both lookups.
			int reflected = angle > 256;
			int q = reflected ? 512 - angle : angle;

			c = mbe_sin_tab[256 - q];
			s = mbe_sin_tab[q];
			if (reflected)
				c = -c;
			if (!inv)
				s = -s;
			twiddled_butterflies(z + 2 * k, len, c, s);
		}
	}
}

void mbe_irfft(int32_t *buf)
{
	// Fold X into Z, then IFFT(Z) gives x[2m] = Re z_m and x[2m+1] = Im z_m.
	int32_t x0 = buf[0], xn = buf[1];
	int32_t *p = buf + 2, *q = buf + 2 * (MBE_FFT_N - 1);

	buf[0] = add32(x0, xn);
	buf[1] = sub32(x0, xn);
	for (int angle = 2; p != q; p += 2, q -= 2, angle += 2) {
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = add32(ar, br), si = sub32(ai, bi);	// X_k + conj X_{N-k}
		int32_t dr = sub32(ar, br), di = add32(ai, bi);	// X_k - conj X_{N-k}
		int32_t c = mbe_sin_tab[256 - angle], s = mbe_sin_tab[angle];
		int32_t er = sub32(mulq15(dr, c), mulq15(di, s));
		int32_t ei = add32(mulq15(dr, s), mulq15(di, c));

		p[0] = sub32(sr, ei);
		p[1] = add32(si, er);
		q[0] = add32(sr, ei);
		q[1] = sub32(er, si);
	}
	// The midpoint pairs with itself, with c = 0 and s = 32767.
	int32_t t = mulq15(add32(p[1], p[1]), mbe_sin_tab[256]);

	p[0] = add32(p[0], p[0]);
	p[1] = sub32(0, t);
	cfft(buf, 1);
}

void mbe_rfft(int32_t *buf)
{
	// FFT the even/odd sample pairs into Z, then unfold Z into the half spectrum X.
	int32_t z0r, z0i;
	int32_t *p = buf + 2, *q = buf + 2 * (MBE_FFT_N - 1);

	cfft(buf, 0);
	z0r = buf[0];
	z0i = buf[1];
	buf[0] = add32(z0r, z0i);
	buf[1] = sub32(z0r, z0i);
	for (int angle = 2; p != q; p += 2, q -= 2, angle += 2) {
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = add32(ar, br) >> 1, si = sub32(ai, bi) >> 1;
		int32_t dr = sub32(ar, br) >> 1, di = add32(ai, bi) >> 1;
		int32_t c = mbe_sin_tab[256 - angle], s = -mbe_sin_tab[angle];
		int32_t er = sub32(mulq15(dr, c), mulq15(di, s));
		int32_t ei = add32(mulq15(dr, s), mulq15(di, c));

		p[0] = add32(sr, ei);
		p[1] = sub32(si, er);
		q[0] = sub32(sr, ei);
		q[1] = sub32(sub32(0, si), er);
	}
	int32_t di = add32(p[1], p[1]) >> 1;

	p[0] = add32(p[0], p[0]) >> 1;
	p[1] = mulq15(di, -mbe_sin_tab[256]);
}
