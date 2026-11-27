// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#ifndef C2FX_PORT_H
#define C2FX_PORT_H

#include "config.h"
#if CONFIG_M17_VOICE && CONFIG_DEBUGLINK
#include <stdbool.h>
#include <stdint.h>

enum {
	C2FX_HOOK_COS,
	C2FX_HOOK_EXP2,
	C2FX_HOOK_LOG2,
	C2FX_HOOK_RFFT,
	C2FX_HOOK_IRFFT,
	C2FX_HOOK_COUNT
};

extern volatile bool c2fx_port_profile;
extern uint32_t c2fx_port_cycles[C2FX_HOOK_COUNT];
extern uint32_t c2fx_port_calls[C2FX_HOOK_COUNT];

void c2fx_port_profile_reset(void);
#endif
#endif