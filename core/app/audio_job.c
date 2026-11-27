// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include "app/audio_job.h"

#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stddef.h>

static void (*pending_job)(void);

bool audio_job_submit(TaskHandle_t task, void (*job)(void))
{
	taskENTER_CRITICAL();
	bool queued = pending_job == NULL;
	if (queued)
		pending_job = job;
	taskEXIT_CRITICAL();

	if (queued)
		xTaskNotifyGive(task);
	return queued;
}

// Keep the dispatcher as a distinct call site for stack_check_edges.txt.
__attribute__((noinline)) void audio_job_service(void)
{
	taskENTER_CRITICAL();
	void (*job)(void) = pending_job;
	pending_job = NULL;
	taskEXIT_CRITICAL();

	if (job != NULL)
		job();
}
#endif
