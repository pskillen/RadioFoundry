// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef RFOUNDRY_AUDIO_JOB_H
#define RFOUNDRY_AUDIO_JOB_H

#include "config.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"

// One pending job for the board's audio task, submitted by the radio task.
bool audio_job_submit(TaskHandle_t task, void (*job)(void));
void audio_job_service(void);
#endif
#endif
