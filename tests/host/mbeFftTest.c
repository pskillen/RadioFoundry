// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mbe/mbe_dsp.h"
#include "dsp/fxp.h"

// Reference from before the FFT optimization. Compile this test with -fwrapv for full-width inputs.
// b is a Q15 twiddle from the quarter wave, a 16-bit factor: fxp_mulshr16's two 32-bit products give fxp_mulshr's result
static inline __attribute__((always_inline)) int32_t mulq15(int32_t a, int32_t b) { return fxp_mulshr16(a, b, 15); }

// Complex FFT of N points, radix 2, decimation in time, no scaling; the inverse (e^{+i}) when inv. The k = 0 and
// k = half/2 butterflies of each stage need no multiply.
static void cfft(int32_t *z, int inv)
{
	const int n = MBE_FFT_N;

	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;

		for (; j & bit; bit >>= 1)
			j ^= bit;
		j |= bit;
		if (i < j) {
			int32_t t = z[2 * i];

			z[2 * i] = z[2 * j];
			z[2 * j] = t;
			t = z[2 * i + 1];
			z[2 * i + 1] = z[2 * j + 1];
			z[2 * j + 1] = t;
		}
	}
	for (int len = 2, step = 512; len <= n; len <<= 1, step >>= 1) {
		int half = len >> 1;

		for (int i = 0; i < n; i += len) {
			int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
			int32_t tr = b[0], ti = b[1];

			b[0] = a[0] - tr;
			b[1] = a[1] - ti;
			a[0] += tr;
			a[1] += ti;
		}
		if (half < 2)
			continue;
		for (int i = half >> 1; i < n; i += len) {
			int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
			int32_t tr = inv ? -b[1] : b[1], ti = inv ? b[0] : -b[0];

			b[0] = a[0] - tr;
			b[1] = a[1] - ti;
			a[0] += tr;
			a[1] += ti;
		}
		for (int k = 1; k < half; k++) {
			int32_t c, s;

			if (2 * k == half)
				continue;
			c = mbe_sin_idx((uint32_t)(k * step + 256));
			s = mbe_sin_idx((uint32_t)(k * step));
			if (!inv)
				s = -s;
			for (int i = k; i < n; i += len) {
				int32_t *a = z + 2 * i, *b = z + 2 * (i + half);
				int32_t tr = mulq15(b[0], c) - mulq15(b[1], s);
				int32_t ti = mulq15(b[0], s) + mulq15(b[1], c);

				b[0] = a[0] - tr;
				b[1] = a[1] - ti;
				a[0] += tr;
				a[1] += ti;
			}
		}
	}
}

static void ref_mbe_irfft(int32_t *buf)
{
	// Fold the half spectrum X[0..N] into N complex points Z_k = (X_k + conj X_{N-k}) + i e^{i pi k/N} (X_k - conj X_{N-k}),
	// then z = IFFT(Z) gives x[2m] = Re z_m and x[2m+1] = Im z_m.
	const int n = MBE_FFT_N;
	int32_t x0 = buf[0], xn = buf[1];

	buf[0] = x0 + xn;
	buf[1] = x0 - xn;
	for (int k = 1; k <= n / 2; k++) {
		int32_t *p = buf + 2 * k, *q = buf + 2 * (n - k);
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = ar + br, si = ai - bi;		// X_k + conj X_{N-k}
		int32_t dr = ar - br, di = ai + bi;		// X_k - conj X_{N-k}
		int32_t c = mbe_sin_idx((uint32_t)(k * 512 / n + 256)), s = mbe_sin_idx((uint32_t)(k * 512 / n));
		int32_t er = mulq15(dr, c) - mulq15(di, s), ei = mulq15(dr, s) + mulq15(di, c);	// e^{i pi k/N} * d

		p[0] = sr - ei;
		p[1] = si + er;
		if (k != n - k) {
			q[0] = sr + ei;
			q[1] = -si + er;
		}
	}
	cfft(buf, 1);
}

static void ref_mbe_rfft(int32_t *buf)
{
	// z_m = x[2m] + i x[2m+1], Z = FFT(z); X_k = (Z_k + conj Z_{N-k}) / 2 - i e^{-i pi k/N} (Z_k - conj Z_{N-k}) / 2
	const int n = MBE_FFT_N;
	int32_t z0r, z0i;

	cfft(buf, 0);
	z0r = buf[0];
	z0i = buf[1];
	buf[0] = z0r + z0i;				// X_0 and X_N, both real
	buf[1] = z0r - z0i;
	for (int k = 1; k <= n / 2; k++) {
		int32_t *p = buf + 2 * k, *q = buf + 2 * (n - k);
		int32_t ar = p[0], ai = p[1], br = q[0], bi = q[1];
		int32_t sr = (ar + br) >> 1, si = (ai - bi) >> 1;	// (Z_k + conj Z_{N-k}) / 2
		int32_t dr = (ar - br) >> 1, di = (ai + bi) >> 1;	// (Z_k - conj Z_{N-k}) / 2
		int32_t c = mbe_sin_idx((uint32_t)(k * 512 / n + 256)), s = -mbe_sin_idx((uint32_t)(k * 512 / n));
		int32_t er = mulq15(dr, c) - mulq15(di, s), ei = mulq15(dr, s) + mulq15(di, c);	// e^{-i pi k/N} * d

		p[0] = sr + ei;
		p[1] = si - er;
		if (k != n - k) {
			q[0] = sr - ei;
			q[1] = -si - er;
		}
	}
}

typedef void (*fft_fn)(int32_t *);

static void check(const char *direction, fft_fn fft, fft_fn ref, const int32_t *input, const char *pattern, int trial)
{
	int32_t actual[2 * MBE_FFT_N], expected[2 * MBE_FFT_N];

	memcpy(actual, input, sizeof(actual));
	memcpy(expected, input, sizeof(expected));
	fft(actual);
	ref(expected);
	for (int i = 0; i < 2 * MBE_FFT_N; i++) {
		if (actual[i] != expected[i]) {
			fprintf(stderr, "%s %s trial %d word %d: got %ld, expected %ld\n",
				direction, pattern, trial, i, (long)actual[i], (long)expected[i]);
			exit(EXIT_FAILURE);
		}
	}
}

static void check_both(const int32_t *input, const char *pattern, int trial)
{
	check("forward", mbe_rfft, ref_mbe_rfft, input, pattern, trial);
	check("inverse", mbe_irfft, ref_mbe_irfft, input, pattern, trial);
}

int main(void)
{
	uint32_t seed = 0x4A1D3C89u;
	int32_t input[2 * MBE_FFT_N] = { 0 };

	check_both(input, "zero", 0);
	for (int i = 0; i < 2 * MBE_FFT_N; i++) {
		input[i] = INT32_MAX;
		check_both(input, "positive impulse", i);
		input[i] = INT32_MIN;
		check_both(input, "negative impulse", i);
		input[i] = 0;
	}
	for (int i = 0; i < 2 * MBE_FFT_N; i++)
		input[i] = 1024;
	check_both(input, "DC", 0);
	check("inverse", mbe_irfft, ref_mbe_irfft, (int32_t[2 * MBE_FFT_N]){ 1024, 0 }, "DC bin", 0);
	for (int i = 0; i < 2 * MBE_FFT_N; i++)
		input[i] = i & 1 ? -1024 : 1024;
	check_both(input, "Nyquist", 0);
	check("inverse", mbe_irfft, ref_mbe_irfft, (int32_t[2 * MBE_FFT_N]){ 0, 1024 }, "Nyquist bin", 0);
	for (int i = 0; i < 2 * MBE_FFT_N; i++)
		input[i] = INT32_MAX;
	check_both(input, "all max", 0);
	for (int i = 0; i < 2 * MBE_FFT_N; i++)
		input[i] = INT32_MIN;
	check_both(input, "all min", 0);
	for (int trial = 0; trial < 2000; trial++) {
		for (int i = 0; i < 2 * MBE_FFT_N; i++)
			input[i] = (int32_t)mbe_rand(&seed);
		check_both(input, "full-scale random", trial);
		for (int i = 0; i < 2 * MBE_FFT_N; i++)
			input[i] = (int32_t)(mbe_rand(&seed) & 255u) - 128;
		check_both(input, "small-scale random", trial);
	}
	puts("MBE FFT: forward and inverse match the reference on 4,000 random vectors and edge cases");
	return 0;
}
