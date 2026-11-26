// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef M17_BENCH_STUB_FREERTOS_H
#define M17_BENCH_STUB_FREERTOS_H

#include <stdint.h>

typedef uint32_t StackType_t;

#define taskENTER_CRITICAL() ((void)0)
#define taskEXIT_CRITICAL() ((void)0)

#endif
