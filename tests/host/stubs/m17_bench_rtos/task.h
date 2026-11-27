// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef M17_BENCH_STUB_TASK_H
#define M17_BENCH_STUB_TASK_H

typedef void *TaskHandle_t;

void xTaskNotifyGive(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);

void vTaskSuspendAll(void);
int xTaskResumeAll(void);

void test_enter_critical(void);
void test_exit_critical(void);

#undef taskENTER_CRITICAL
#undef taskEXIT_CRITICAL
#define taskENTER_CRITICAL() test_enter_critical()
#define taskEXIT_CRITICAL() test_exit_critical()

#endif
