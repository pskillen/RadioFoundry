// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <string.h>
#include "app/task_cpu.h"
#include "hal_time.h"
#include "FreeRTOS.h"
#include "task.h"

_Static_assert(configMAX_TASK_NAME_LEN <= TASK_STACK_NAME_LEN + 1, "task CPU name buffer is too short");

static uint32_t last_cycles;
static uint64_t clock_cycles;

void task_cpu_clock_start(void)
{
	last_cycles = hal_cycles();
	clock_cycles = 0;
}

uint64_t task_cpu_clock(void)
{
	uint32_t now = hal_cycles();

	// FreeRTOS drops an interval if its clock goes backward. The RTOS tick samples even if no task
	// switches for a whole hardware wrap. All readers run with the RTOS tick and scheduler masked.
	clock_cycles += (uint32_t)(now - last_cycles);
	last_cycles = now;
	return clock_cycles;
}

typedef struct {
	UBaseType_t id;
	uint32_t cycles;
} task_counter_t;

static task_counter_t previous[TASK_STACK_MAX];
static unsigned previous_count;
static uint64_t previous_time;
static bool have_previous;

typedef struct {
	task_cpu_sample_t *sample;
	UBaseType_t ids[TASK_STACK_MAX];
	TaskHandle_t idle;
} snapshot_t;

static void collect_task(TaskHandle_t task, eTaskState state, void *ctx)
{
	snapshot_t *snapshot = ctx;
	task_cpu_sample_t *sample = snapshot->sample;

	if (state == eDeleted) {
		return;
	}
	if (sample->count == TASK_STACK_MAX) {
		sample->omitted++;
		return;
	}

	TaskStatus_t info;
	unsigned i = sample->count++;
	task_cpu_row_t *row = &sample->rows[i];

	// Use the kernel's creation ID: a newly allocated task can reuse a deleted task's handle and name.
	vTaskGetInfo(task, &info, pdFALSE, eReady);
	memcpy(row->name, info.pcTaskName, configMAX_TASK_NAME_LEN);
	row->name[configMAX_TASK_NAME_LEN - 1] = '\0';
	row->cycles = (uint32_t)info.ulRunTimeCounter;
	row->previous = 0;
	snapshot->ids[i] = info.xTaskNumber;
	for (unsigned j = 0; j < previous_count; j++) {
		if (previous[j].id == info.xTaskNumber) {
			row->previous = previous[j].cycles;
			break;
		}
	}
	if (task == snapshot->idle) {
		sample->idle = i;
	}
}

void task_cpu_snapshot(task_cpu_sample_t *sample)
{
	uint64_t now;
	snapshot_t snapshot = { .sample = sample, .idle = xTaskGetIdleTaskHandle() };

	sample->count = 0;
	sample->omitted = 0;
	sample->idle = TASK_STACK_MAX;

	// Keep the baseline update atomic across both transports, then format with scheduling enabled.
	vTaskSuspendAll();
	(void)uxTaskCallForEachTask(collect_task, &snapshot, NULL);
	taskENTER_CRITICAL();
	now = task_cpu_clock();
	taskEXIT_CRITICAL();
	uint64_t elapsed = now - previous_time;
	sample->window_cycles = have_previous && elapsed <= UINT32_MAX ? (uint32_t)elapsed : 0;
	previous_time = now;
	have_previous = true;
	previous_count = sample->count;
	for (unsigned i = 0; i < sample->count; i++) {
		previous[i] = (task_counter_t){ snapshot.ids[i], sample->rows[i].cycles };
	}
	(void)xTaskResumeAll();
}
