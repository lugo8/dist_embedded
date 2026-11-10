/* time synchronization; see timesync.h */
#include <string.h>
#include "timesync.h"

static int64_t clamp64(int64_t v, int64_t lo, int64_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

void ts_init(struct timesync *ts)
{
	memset(ts, 0, sizeof(*ts));
}

int64_t ts_global(const struct timesync *ts, int64_t local)
{
	if (!ts->ever) {
		return local; /* never synced (cold-start master): our own clock is the time */
	}
	int64_t dt = local - ts->l0;

	return ts->g0 + dt + dt * (ts->ppb + ts->slew_ppb) / 1000000000;
}

int64_t ts_extend(const struct timesync *ts, int64_t local, uint32_t m32)
{
	int64_t g = ts_global(ts, local);

	return g + (int32_t)(m32 - (uint32_t)g);
}

/* re-anchor the model at `local` without changing its value there */
static void reanchor(struct timesync *ts, int64_t local)
{
	ts->g0 = ts_global(ts, local);
	ts->l0 = local;
}

static void sample(struct timesync *ts, int64_t l, int64_t f)
{
	if (!ts->ever) {
		ts->ever = ts->locked = true;
		ts->g0 = l;
		ts->l0 = f;
		ts->ppb = ts->slew_ppb = 0;
		ts->base_l = l;
		ts->base_f = f;
		ts->base_valid = true;
		ts->prev_l = l;
		ts->prev_f = f;
		ts->last_sync_local = f;
		ts->steps++;
		ts->syncs++;
		return;
	}
	/* the handout's ratio over one heartbeat, for printing */
	if (f > ts->prev_f) {
		ts->pair_ppb = (int32_t)clamp64(((l - ts->prev_l) - (f - ts->prev_f)) * 1000000000 /
						(f - ts->prev_f), -TS_MAX_DRIFT_PPB, TS_MAX_DRIFT_PPB);
	}
	ts->prev_l = l;
	ts->prev_f = f;

	/* drift over a long baseline, averaged */
	if (!ts->base_valid) {
		ts->base_l = l;
		ts->base_f = f;
		ts->base_valid = true;
	} else if (f - ts->base_f >= TS_BASELINE_US) {
		int64_t df = f - ts->base_f;
		int64_t meas = ((l - ts->base_l) - df) * 1000000000 / df;

		if (meas > -TS_MAX_DRIFT_PPB && meas < TS_MAX_DRIFT_PPB) {
			ts->ppb += (int32_t)((meas - ts->ppb) / 2);
		}
		ts->base_l = l;
		ts->base_f = f;
	}

	int64_t err = l - ts_global(ts, f);

	ts->last_error_us = (int32_t)clamp64(err, INT32_MIN, INT32_MAX);
	ts->syncs++;
	ts->last_sync_local = f;
	ts->locked = true;
	if (err > TS_STEP_US || err < -TS_STEP_US) {
		/* far off (a different timeline): step, and restart the drift baseline */
		ts->g0 = l;
		ts->l0 = f;
		ts->slew_ppb = 0;
		ts->base_l = l;
		ts->base_f = f;
		ts->steps++;
		return;
	}
	reanchor(ts, f);
	ts->slew_ppb = (int32_t)clamp64(err * 1000000000 / TS_SLEW_US, -TS_MAX_SLEW_PPB,
					TS_MAX_SLEW_PPB);
}

bool ts_on_ref(struct timesync *ts, int bus, uint8_t cycle, bool followup, bool relayed,
	       uint32_t m32, int64_t rx_us)
{
	struct ts_rx *prev = &ts->rx[bus];
	bool used = false;

	if (followup) {
		/* m32 is when REF cycle-1 finished on this bus at the master */
		uint8_t want = (uint8_t)(cycle - 1);

		if (prev->valid && prev->cycle == want && !(ts->cycle_valid && ts->last_cycle == want)) {
			int64_t l = ts_extend(ts, prev->local_us, m32) + TS_RX_DELAY_US +
				    (prev->relayed ? TS_RELAY_DELAY_US : 0);

			sample(ts, l, prev->local_us);
			ts->last_cycle = want;
			ts->cycle_valid = true;
			used = true;
		}
	} else if (!(ts->cycle_valid && ts->last_cycle == cycle)) {
		int64_t l = ts_extend(ts, rx_us, m32) + TS_QUEUE_DELAY_US +
			    (relayed ? TS_RELAY_DELAY_US : 0);

		sample(ts, l, rx_us);
		ts->last_cycle = cycle;
		ts->cycle_valid = true;
		used = true;
	}
	*prev = (struct ts_rx){ true, cycle, relayed, rx_us };
	return used;
}

void ts_become_master(struct timesync *ts, int64_t local)
{
	if (ts->ever) {
		reanchor(ts, local);
		ts->slew_ppb = 0; /* keep the drift: we keep running on the old master's time */
		/* ...unless it can't be a crystal: then it was measured on bad samples, and a new
		 * master must not hand a wrong rate to the whole vehicle */
		if (ts->ppb > TS_MASTER_MAX_PPB || ts->ppb < -TS_MASTER_MAX_PPB) {
			ts->ppb = 0;
		}
	}
	ts->rx[0].valid = ts->rx[1].valid = false;
	ts->cycle_valid = false;
}

void ts_check(struct timesync *ts, int64_t local)
{
	if (ts->locked && local - ts->last_sync_local > TS_LOCK_LOST_US) {
		ts->locked = false;
	}
	/* a finished slew must not keep running: it was sized for one sync interval's worth
	 * of correction; past that the error it was correcting is gone */
	if (ts->slew_ppb && local - ts->l0 > TS_SLEW_US) {
		reanchor(ts, ts->l0 + TS_SLEW_US); /* same value there, then drift rate only */
		ts->slew_ppb = 0;
	}
}
