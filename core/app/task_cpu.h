// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef TASK_CPU_H
#define TASK_CPU_H

#include "app/task_stack.h"

typedef struct {
	char name[TASK_STACK_NAME_LEN + 1];
	uint32_t cycles;
	uint32_t previous;
} task_cpu_row_t;

typedef struct {
	task_cpu_row_t rows[TASK_STACK_MAX];
	uint32_t window_cycles;		// zero before a baseline or if the interval cannot fit in 32 bits
	unsigned count;
	unsigned omitted;
	unsigned idle;			// row index, or TASK_STACK_MAX
} task_cpu_sample_t;

// The FreeRTOS run-time stats clock, extended from hal_cycles() at each read.
void task_cpu_clock_start(void);
uint64_t task_cpu_clock(void);
// Capture live tasks and advance the shared baseline before returning.
void task_cpu_snapshot(task_cpu_sample_t *sample);

#endif
