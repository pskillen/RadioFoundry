// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef TASK_CPU_STUB_TASK_H
#define TASK_CPU_STUB_TASK_H

#include "FreeRTOS.h"

typedef struct fake_task *TaskHandle_t;
typedef enum { eReady, eDeleted } eTaskState;

typedef struct {
	const char *pcTaskName;
	uint64_t ulRunTimeCounter;
	UBaseType_t xTaskNumber;
} TaskStatus_t;

typedef void (*TaskStatusCallbackFunction_t)(TaskHandle_t task, eTaskState state, void *ctx);

TaskHandle_t xTaskGetIdleTaskHandle(void);
void vTaskSuspendAll(void);
int xTaskResumeAll(void);
UBaseType_t uxTaskCallForEachTask(TaskStatusCallbackFunction_t callback, void *ctx, uint64_t *total);
void vTaskGetInfo(TaskHandle_t task, TaskStatus_t *info, int read_stack, eTaskState state);

#endif
