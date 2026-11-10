/*
 * Time synchronization (Lab 5 Part 6). Plain C, integer math, no OS.
 *
 * Every node keeps a model of the global time as a function of its local clock:
 *     global(local) = g0 + (local - l0) + (local - l0) * ppb / 1e9
 * anchored at the last sync. The leader is the time master: its global time is just its
 * own model run forward, and it puts that time in every REF.
 *
 * 6.1 clock sync: a follower pairs the master's time with its own receive time of the same
 *     REF (L1, F1). The follow-up form (FLAG_REF_FOLLOWUP) uses the time the previous REF
 *     finished on this bus at the master and the time it was received here: every node sees
 *     the end of a frame at the same instant, so the network delay that is left is the
 *     difference in interrupt latency, a small constant (TS_RX_DELAY_US). The simple form
 *     uses the time the master queued the REF plus a typical frame time (TS_QUEUE_DELAY_US).
 * 6.2 drift: rate = (L2 - L1) / (F2 - F1), measured over at least TS_BASELINE_US so that
 *     receive jitter of a few us is a few ppm of error instead of hundreds, then averaged.
 *     The handout's per-heartbeat ratio is also kept (ts.pair_ppb) for printing.
 * Between syncs the model runs at that rate. A phase error is not stepped away: it is
 * slewed out over TS_SLEW_US by adding a small rate on top, so global time never jumps
 * (the checkoff allows no step larger than 100 us at a leader handover). Only the first
 * sync, or an error over TS_STEP_US, steps.
 */
#ifndef TIMESYNC_H
#define TIMESYNC_H

#include <stdbool.h>
#include <stdint.h>

#define TS_BASELINE_US    1000000 /* drift measured over >= 1 s */
#define TS_SLEW_US        100000  /* a phase error is worked off over 100 ms */
#define TS_MAX_SLEW_PPB   1000000 /* at most 1000 ppm on top of the drift */
#define TS_MAX_DRIFT_PPB  500000  /* reject drift estimates beyond 500 ppm */
#define TS_STEP_US        2000    /* errors larger than this step instead of slewing */
#define TS_LOCK_LOST_US   100000  /* no sync this long: SYNC off, free-run */
#ifndef TS_RX_DELAY_US
#define TS_RX_DELAY_US    0       /* follow-up form: receive minus transmit-done latency */
#endif
#ifndef TS_QUEUE_DELAY_US
#define TS_QUEUE_DELAY_US 250     /* simple form: about one 8-byte frame at 500 kbit/s */
#endif
#ifndef TS_RELAY_DELAY_US
#define TS_RELAY_DELAY_US 300     /* a relayed copy went out one frame + relay latency later */
#endif

struct ts_rx {
	bool valid;
	uint8_t cycle;
	bool relayed;
	int64_t local_us; /* when that REF finished arriving on this bus */
};

struct timesync {
	bool locked;      /* has synced at least once and not lost it */
	bool ever;
	int64_t g0, l0;   /* model anchor */
	int32_t ppb;      /* measured drift of the master relative to us */
	int32_t slew_ppb; /* temporary extra rate working off the last phase error */
	int64_t last_sync_local;
	int32_t last_error_us; /* master minus model at the last sync, before correcting */
	int32_t pair_ppb;      /* (L2-L1)/(F2-F1) - 1 for the last two consecutive syncs */
	int64_t prev_l, prev_f;
	int64_t base_l, base_f; /* baseline start for the drift estimate */
	bool base_valid;
	uint8_t last_cycle;
	bool cycle_valid;
	struct ts_rx rx[2]; /* per bus: the last REF received, for the follow-up pairing */
	uint32_t syncs, steps;
};

void ts_init(struct timesync *ts);
int64_t ts_global(const struct timesync *ts, int64_t local_us);
/* the master's 32-bit REF time extended to 64 bits around our own global time */
int64_t ts_extend(const struct timesync *ts, int64_t local_us, uint32_t master32);
/*
 * A REF from the master arrived on `bus` at local time `rx_us` (the copy's own receive
 * time, for each bus copy). Returns true if it moved the model.
 */
bool ts_on_ref(struct timesync *ts, int bus, uint8_t cycle, bool followup, bool relayed,
	       uint32_t master32, int64_t rx_us);
/* becoming the master: keep the model (no jump), stop slewing towards the old master */
void ts_become_master(struct timesync *ts, int64_t local_us);
/* call periodically: drops the lock when no REF arrives */
void ts_check(struct timesync *ts, int64_t local_us);

#endif /* TIMESYNC_H */
