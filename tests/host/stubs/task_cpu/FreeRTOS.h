// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef TASK_CPU_STUB_FREERTOS_H
#define TASK_CPU_STUB_FREERTOS_H

#include <stdint.h>

#define configMAX_TASK_NAME_LEN 12
#define pdFALSE 0

typedef unsigned UBaseType_t;

void taskENTER_CRITICAL(void);
void taskEXIT_CRITICAL(void);

#endif
