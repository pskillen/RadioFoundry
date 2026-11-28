// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app/task_cpu.h"
#include "debuglink/debuglink.h"
#include "hal_time.h"
#include "task.h"

static struct fake_task {
	char name[configMAX_TASK_NAME_LEN];
	uint64_t cycles;
	unsigned id;
	eTaskState state;
} tasks[TASK_STACK_MAX + 1];
static unsigned task_count, suspended, masked;
static uint32_t timer;
static char output[1001];

uint32_t hal_cycles(void)
{
	return timer;
}

void taskENTER_CRITICAL(void)
{
	masked++;
}

void taskEXIT_CRITICAL(void)
{
	assert(masked-- == 1);
}

TaskHandle_t xTaskGetIdleTaskHandle(void)
{
	return &tasks[0];
}

void vTaskSuspendAll(void)
{
	suspended++;
}

int xTaskResumeAll(void)
{
	assert(suspended > 0);
	suspended--;
	return 0;
}

UBaseType_t uxTaskCallForEachTask(TaskStatusCallbackFunction_t callback, void *ctx, uint64_t *total)
{
	assert(suspended == 1 && total == NULL);
	for (unsigned i = 0; i < task_count; i++) {
		callback(&tasks[i], tasks[i].state, ctx);
	}
	return task_count;
}

void vTaskGetInfo(TaskHandle_t task, TaskStatus_t *info, int read_stack, eTaskState state)
{
	assert(suspended == 1 && read_stack == pdFALSE && state == eReady);
	*info = (TaskStatus_t){ task->name, task->cycles, task->id };
}

static size_t render(void)
{
	dl_text_t t;

	dl_text_init(&t, output, sizeof(output) - 1);
	dl_text(&t, "section=tasks_cpu\n");
	debuglink_view_tasks_cpu(&t);
	assert(!t.full && suspended == 0 && masked == 0);
	output[t.len] = '\0';
	return t.len;
}

int main(void)
{
	timer = UINT32_MAX - 200;
	task_cpu_clock_start();
	tasks[0] = (struct fake_task){ "IDLE", UINT32_MAX - 199, 1, eReady };
	tasks[1] = (struct fake_task){ "Tmr Svc", 300, 2, eReady };
	task_count = 2;

	render();
	assert(strcmp(output, "section=tasks_cpu\ntasks_cpu.cycles=0\ntasks_cpu.omitted=0\ntasks_cpu.idle=0\n"
		      "tcpu.IDLE=0\ntcycles.IDLE=4294967096\ntcpu.Tmr_Svc=0\ntcycles.Tmr_Svc=300\n") == 0);

	// Both the hardware clock and the reported low 32 bits of the idle counter wrap.
	timer += 1000;
	tasks[0].cycles += 600;
	tasks[1].cycles += 400;
	render();
	assert(strcmp(output, "section=tasks_cpu\ntasks_cpu.cycles=1000\ntasks_cpu.omitted=0\ntasks_cpu.idle=600\n"
		      "tcpu.IDLE=600\ntcycles.IDLE=400\ntcpu.Tmr_Svc=400\ntcycles.Tmr_Svc=700\n") == 0);

	// The timer slot is reused. Only the new task's own cycles count.
	timer += 300;
	tasks[0].cycles += 200;
	tasks[1].id = 3;
	tasks[1].cycles = 100;
	tasks[2] = (struct fake_task){ "gone", 1000, 4, eDeleted };
	task_count = 3;
	render();
	assert(strstr(output, "tasks_cpu.cycles=300\n") != NULL);
	assert(strstr(output, "tcpu.IDLE=666\n") != NULL);
	assert(strstr(output, "tcpu.Tmr_Svc=333\n") != NULL);
	assert(strstr(output, "gone") == NULL);

	// Periodic samples retain the full elapsed time, so an oversized window is rejected and rearmed.
	timer += UINT32_MAX;
	assert(task_cpu_clock() == (uint64_t)UINT32_MAX + 1300);
	timer += 1;
	render();
	assert(strstr(output, "tasks_cpu.cycles=0\n") != NULL);

	// A full reply with longest task names and max-width counters still fits.
	task_count = TASK_STACK_MAX + 1;
	timer += 1;
	for (unsigned i = 0; i < task_count; i++) {
		snprintf(tasks[i].name, sizeof(tasks[i].name), "long_task%02u", i);
		tasks[i].cycles = UINT32_MAX;
		tasks[i].id = 10 + i;
		tasks[i].state = eReady;
	}
	assert(render() < 1000);
	assert(strstr(output, "tasks_cpu.cycles=1\n") != NULL);
	assert(strstr(output, "tasks_cpu.omitted=1\n") != NULL);
	assert(strstr(output, "tcpu.long_task15=1000\ntcycles.long_task15=4294967295\n") != NULL);

	puts("taskCpuTest: passed");
	return 0;
}
