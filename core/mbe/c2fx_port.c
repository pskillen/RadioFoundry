// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// External DSP hooks declared in c2fx_dsp.h. Keep the MBE build independent of c2fx headers.
#include "mbe_dsp.h"
#include "config.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include "c2fx_port.h"
#include "hal_time.h"
#endif

_Static_assert(2 * MBE_FFT_N == 512, "c2fx requires a 512-point real FFT");

#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
volatile bool c2fx_port_profile;
uint32_t c2fx_port_cycles[C2FX_HOOK_COUNT];
uint32_t c2fx_port_calls[C2FX_HOOK_COUNT];

void c2fx_port_profile_reset(void)
{
	for (unsigned i = 0; i < C2FX_HOOK_COUNT; i++) {
		c2fx_port_cycles[i] = 0;
		c2fx_port_calls[i] = 0;
	}
}
#endif

int32_t c2fx_cos(uint32_t phase)
{
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	if (c2fx_port_profile) {
		uint32_t start = hal_cycles();
		int32_t ret = mbe_cos_i(phase);

		c2fx_port_cycles[C2FX_HOOK_COS] += hal_cycles() - start;
		c2fx_port_calls[C2FX_HOOK_COS]++;
		return ret;
	}
#endif
	return mbe_cos_i(phase);
}

int32_t c2fx_exp2_q11(int32_t x)
{
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	if (c2fx_port_profile) {
		uint32_t start = hal_cycles();
		int32_t ret = mbe_exp2_q11(x);

		c2fx_port_cycles[C2FX_HOOK_EXP2] += hal_cycles() - start;
		c2fx_port_calls[C2FX_HOOK_EXP2]++;
		return ret;
	}
#endif
	return mbe_exp2_q11(x);
}

int32_t c2fx_log2_q11(uint32_t x)
{
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	if (c2fx_port_profile) {
		uint32_t start = hal_cycles();
		int32_t ret = mbe_log2_q11(x);

		c2fx_port_cycles[C2FX_HOOK_LOG2] += hal_cycles() - start;
		c2fx_port_calls[C2FX_HOOK_LOG2]++;
		return ret;
	}
#endif
	return mbe_log2_q11(x);
}

void c2fx_rfft512(int32_t *buf)
{
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	if (c2fx_port_profile) {
		uint32_t start = hal_cycles();

		mbe_rfft(buf);
		c2fx_port_cycles[C2FX_HOOK_RFFT] += hal_cycles() - start;
		c2fx_port_calls[C2FX_HOOK_RFFT]++;
		return;
	}
#endif
	mbe_rfft(buf);
}

void c2fx_irfft512(int32_t *buf)
{
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
	if (c2fx_port_profile) {
		uint32_t start = hal_cycles();

		mbe_irfft(buf);
		c2fx_port_cycles[C2FX_HOOK_IRFFT] += hal_cycles() - start;
		c2fx_port_calls[C2FX_HOOK_IRFFT]++;
		return;
	}
#endif
	mbe_irfft(buf);
}