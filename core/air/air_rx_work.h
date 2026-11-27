// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Work counters for the host receive benchmark. They compile out of normal builds.
#ifndef RFOUNDRY_AIR_RX_WORK_H
#define RFOUNDRY_AIR_RX_WORK_H

#ifdef AIR_RX_WORK
#include <stdint.h>

typedef struct {
	uint32_t search, fits, filter_macs, sync_tries, sync_fits, heads, viterbi, viterbi_bits, replay_syms;
} air_rx_work_t;

extern air_rx_work_t air_rx_work;
#define AIR_RX_ADD(field, n) (air_rx_work.field += (n))
#else
#define AIR_RX_ADD(field, n) ((void)0)
#endif

#endif
