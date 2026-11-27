// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef M17_BENCH_STUB_TASK_H
#define M17_BENCH_STUB_TASK_H

typedef void *TaskHandle_t;

void xTaskNotifyGive(TaskHandle_t task);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task);

#endif
