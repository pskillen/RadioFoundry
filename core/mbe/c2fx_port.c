// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// External DSP hooks declared in c2fx_dsp.h. Keep the MBE build independent of c2fx headers.
#include "mbe_dsp.h"

_Static_assert(2 * MBE_FFT_N == 512, "c2fx requires a 512-point real FFT");

int32_t c2fx_cos(uint32_t phase)
{
	return mbe_cos_i(phase);
}

int32_t c2fx_exp2_q11(int32_t x)
{
	return mbe_exp2_q11(x);
}

int32_t c2fx_log2_q11(uint32_t x)
{
	return mbe_log2_q11(x);
}

void c2fx_rfft512(int32_t *buf)
{
	mbe_rfft(buf);
}

void c2fx_irfft512(int32_t *buf)
{
	mbe_irfft(buf);
}
