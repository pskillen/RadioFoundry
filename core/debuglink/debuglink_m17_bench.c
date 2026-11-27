// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "debuglink_m17_bench.h"
#if CONFIG_DEBUGLINK && CONFIG_M17_VOICE
#include "FreeRTOS.h"
#include "task.h"
#include "debuglink/dl_frame.h"
#include "hal_time.h"
#include "m17/m17_bench.h"
#include "radio/radio.h"

#define BENCH_WAIT_MS 2000u

_Static_assert(AUDIO_BLOCK_SAMPLES == C2FX_FRAME_LEN, "one PCM block must hold one decoded frame");

static enum { IDLE, ARMED, RUNNING, DONE, NO_MEMORY, CANCELED } phase;
static m17_bench_result_t last;

static __attribute__((noinline)) uint8_t ready(void)
{
	radio_view_t view;

	radio_get_view(&view);
	if (!(view.m17.flags & RADIO_M17_ON))
		return DL_ST_REFUSED;
	return (view.m17.flags & RADIO_M17_VOICE_MEM) ? DL_ST_OK : DL_ST_UNAVAILABLE;
}

uint8_t debuglink_m17_bench_run(void)
{
	uint8_t status = ready();

	if (status != DL_ST_OK)
		return status;
	taskENTER_CRITICAL();
	if (phase != IDLE) {
		taskEXIT_CRITICAL();
		return DL_ST_BUSY;
	}
	phase = ARMED;
	taskEXIT_CRITICAL();

	uint32_t start = hal_millis();
	for (;;) {
		bool expired = hal_millis() - start >= BENCH_WAIT_MS;

		taskENTER_CRITICAL();
		if (phase == DONE || phase == NO_MEMORY) {
			status = phase == DONE ? DL_ST_OK : DL_ST_UNAVAILABLE;
			phase = IDLE;
			taskEXIT_CRITICAL();
			return status;
		}
		if (expired) {
			phase = phase == RUNNING ? CANCELED : IDLE;
			taskEXIT_CRITICAL();
			return DL_ST_REFUSED;
		}
		taskEXIT_CRITICAL();
		hal_delay_ms(10);
	}
}

bool debuglink_m17_bench_armed(void)
{
	taskENTER_CRITICAL();
	bool armed = phase == ARMED;
	taskEXIT_CRITICAL();
	return armed;
}

bool debuglink_m17_bench_audio(void *dec, void *scratch, int16_t pcm[AUDIO_BLOCK_SAMPLES])
{
	taskENTER_CRITICAL();
	if (phase != ARMED) {
		taskEXIT_CRITICAL();
		return false;
	}
	phase = RUNNING;
	taskEXIT_CRITICAL();

	m17_bench_result_t result;
	bool ran = m17_bench_run(dec, scratch, pcm, &result);

	taskENTER_CRITICAL();
	if (phase == CANCELED) {
		phase = IDLE;
	} else if (!ran) {
		phase = NO_MEMORY;
	} else {
		last = result;
		phase = DONE;
	}
	taskEXIT_CRITICAL();
	return ran;
}

void debuglink_m17_bench_view(dl_text_t *t)
{
	taskENTER_CRITICAL();
	m17_bench_result_t result = last;
	taskEXIT_CRITICAL();
	dl_text(t, "c2.dec_cycles=%u\nc2.dec_crc=0x%08x\nc2.audio_free=%u\nc2.scratch=%u\nc2.dec_bytes=%u\nc2.enc=0\n",
		(unsigned)result.dec_cycles, (unsigned)result.dec_crc, (unsigned)result.audio_free,
		(unsigned)C2FX_SCRATCH_BYTES, (unsigned)C2FX_DEC_BYTES);
}
#endif
