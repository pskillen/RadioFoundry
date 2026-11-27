// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
#include <stdio.h>
#include <stdlib.h>

#include "air/air_session.h"

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "line %d: %s\n", __LINE__, #cond); exit(1); } } while (0)

static air_event_cb_t callback;
static void *callback_ctx;
static bool queued;
static bool permit_start = true;
static unsigned done_count;
static uint8_t observed_reason;

static uint32_t fit(const air_profile_t *profile)
{
	(void)profile;
	return AIR_FIT_TX;
}

static bool open_air(const air_profile_t *profile, air_event_cb_t cb, void *ctx)
{
	(void)profile;
	callback = cb;
	callback_ctx = ctx;
	return true;
}

static bool queue_chunk(const air_tx_chunk_t *chunk)
{
	CHECK(!queued && chunk->nsyms == 4 && (chunk->flags & AIR_TX_LAST));
	queued = true;
	return true;
}

static uint8_t tx_space(void)
{
	return queued ? 0 : 1;
}

static bool tx_begin(void)
{
	CHECK(queued);
	return permit_start;
}

static void dispatch(air_event_kind_t kind, uint32_t arg)
{
	const air_event_t event = { .kind = kind, .arg = arg };
	callback(&event, callback_ctx);
}

static void end_tx(uint32_t reason)
{
	queued = false;
	dispatch(AIR_EV_TX_DONE, reason);
}

static void tx_abort(void)
{
	end_tx(AIR_TX_END_ABORTED);
}

static void close_air(void)
{
	queued = false;
}

static void fill(void *ctx)
{
	air_session_t *session = ctx;
	const uint8_t symbols = 0x55;

	CHECK(air_session_tx(session, &symbols, 0, 4));
	air_session_tx_finish(session);
}

static void done(void *ctx)
{
	const air_session_t *session = ctx;

	CHECK(air_session_tx_state(session) == AIR_TX_DONE);
	observed_reason = session->tx_end_reason;
	done_count++;
}

static void prime(air_session_t *session)
{
	CHECK(air_session_tx_prime(session));
	CHECK(session->tx_end_reason == AIR_TX_END_UNKNOWN);
}

static void check_done(air_session_t *session, air_tx_end_reason_t reason, unsigned count)
{
	CHECK(air_session_tx_state(session) == AIR_TX_DONE);
	CHECK(session->tx_end_reason == reason && observed_reason == reason && done_count == count);
	air_session_tx_clear(session);
	CHECK(air_session_tx_state(session) == AIR_TX_IDLE && session->tx_end_reason == reason);
}

int main(void)
{
	static const hal_air_t air = {
		.fit = fit, .open = open_air, .close = close_air,
		.tx_begin = tx_begin, .tx_queue = queue_chunk, .tx_space = tx_space, .tx_abort = tx_abort,
		.tx_chunk_syms = 8, .tx_queue_depth = 1,
	};
	static const air_mode_t mode = { 0 };
	static const air_session_ops_t ops = { .tx_fill = fill, .tx_done = done };
	uint8_t hard[2 * AIR_SYNC_SYMS_MAX / 4];
	air_session_t session;

	CHECK(air_session_open(&session, &air, &mode, NULL, hard, NULL, 2 * AIR_SYNC_SYMS_MAX, &ops, &session));
	prime(&session);
	CHECK(air_session_tx_go(&session));
	dispatch(AIR_EV_TX_UNDERRUN, 0);
	end_tx(AIR_TX_END_DRAINED);
	check_done(&session, AIR_TX_END_DRAINED, 1);
	CHECK(session.stats.tx_underruns == 1);

	prime(&session);
	CHECK(air_session_tx_go(&session));
	air_session_tx_abort(&session);
	check_done(&session, AIR_TX_END_ABORTED, 2);

	prime(&session);
	CHECK(air_session_tx_go(&session));
	end_tx(AIR_TX_END_STALLED);
	check_done(&session, AIR_TX_END_STALLED, 3);
	dispatch(AIR_EV_TX_DONE, AIR_TX_END_DRAINED);
	CHECK(session.tx_end_reason == AIR_TX_END_STALLED && done_count == 3);

	prime(&session);
	CHECK(air_session_tx_go(&session));
	end_tx(AIR_TX_END_UNKNOWN);
	check_done(&session, AIR_TX_END_UNKNOWN, 4);

	prime(&session);
	CHECK(air_session_tx_go(&session));
	end_tx(0x101u);
	check_done(&session, AIR_TX_END_UNKNOWN, 5);

	prime(&session);
	permit_start = false;
	CHECK(!air_session_tx_go(&session));
	CHECK(air_session_tx_state(&session) == AIR_TX_DONE);
	CHECK(session.tx_end_reason == AIR_TX_END_UNKNOWN && done_count == 5);
	air_session_close(&session);
	puts("airSessionTxEndTest: passed");
	return 0;
}
