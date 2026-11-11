/*
 * Time sync with three drifting crystals: REF every 20 ms in follow-up form, random
 * interrupt latency, a leader handover with an election gap, and a stretch where the
 * follower only gets relayed copies. Checks the PPS spread and the step at the handover.
 */
#include <stdio.h>
#include <stdlib.h>
#include "timesync.h"

static const double ppm[4] = { 0, +40, -35, +15 };  /* crystal error per node */
/* boot times: the master (N1, the Pi) has been up 40 min, the STM32s were just reset: the
 * case where a 32-bit master time made followers guess the wrong wrap */
static const int64_t off[4] = { 0, 2400000000LL, 5123456, 777777 };
static struct timesync ts[4];
static int failures;

static int64_t local_of(int n, int64_t t)
{
	return off[n] + t + (int64_t)(t * ppm[n] / 1e6);
}

static int64_t global_of(int n, int64_t t)
{
	return ts_global(&ts[n], local_of(n, t));
}

/* rx latency 5..30 us, relayed copies one frame + relay processing later */
static int64_t rx_time(int n, int64_t eof, bool relayed)
{
	return local_of(n, eof + 5 + rand() % 26 + (relayed ? 250 + 30 + rand() % 40 : 0));
}

int main(void)
{
	int master = 1;
	uint8_t cycle = 0;
	int64_t prev_eof_global = 0; /* master's global time at the previous REF's end */
	int64_t max_spread = 0, max_step = 0, max_spread_relay = 0;
	int64_t last_g[4] = { 0 };

	for (int n = 1; n <= 3; n++) {
		ts_init(&ts[n]);
	}
	srand(1);
	for (int64_t t = 0; t <= 30000000; t += 1000) { /* 30 s in 1 ms steps */
		bool gap = t >= 10000000 && t < 10100000;   /* election after the leader dies */
		bool relayed = t >= 20000000 && t < 25000000;

		if (t == 10100000) {
			master = 2; /* node 2 wins: continues on its own model */
			ts_become_master(&ts[2], local_of(2, t));
		}
		if (t % 20000 == 0 && !gap) {
			int64_t eof = t + rand() % 800; /* when it actually got on the bus */

			for (int n = 1; n <= 3; n++) {
				if (n == master || (n == 1 && t >= 10000000)) {
					continue; /* node 1 is off after 10 s */
				}
				bool rel = relayed && n == 3;

				ts_on_ref(&ts[n], rel ? 1 : 0, cycle, true, rel, (uint64_t)prev_eof_global,
					  rx_time(n, eof, rel));
			}
			prev_eof_global = global_of(master, eof);
			cycle++;
		}
		for (int n = 1; n <= 3; n++) {
			ts_check(&ts[n], local_of(n, t));
		}
		/* spread of global time across live nodes, once settled */
		int64_t lo = INT64_MAX, hi = INT64_MIN;

		for (int n = (t >= 10000000 ? 2 : 1); n <= 3; n++) {
			int64_t g = global_of(n, t);

			lo = g < lo ? g : lo;
			hi = g > hi ? g : hi;
			if (t > 3000000 && last_g[n]) {
				int64_t step = (g - last_g[n]) - 1000;

				step = step < 0 ? -step : step;
				if (step > max_step) {
					max_step = step;
				}
			}
			last_g[n] = g;
		}
		if (t > 3000000) {
			if (relayed && hi - lo > max_spread_relay) {
				max_spread_relay = hi - lo;
			} else if (!relayed && hi - lo > max_spread) {
				max_spread = hi - lo;
			}
		}
		if (t % 5000000 == 0) {
			printf("  t=%2llds master N%d  spread %4lld us  drift N2 %+7.2f ppm N3 %+7.2f ppm"
			       "  err N3 %+4d us\n",
			       (long long)(t / 1000000), master, (long long)(hi - lo), ts[2].ppb / 1000.0,
			       ts[3].ppb / 1000.0, ts[3].last_error_us);
		}
	}
	printf("  PPS spread after lock: %lld us direct, %lld us with N3 on relayed REFs\n",
	       (long long)max_spread, (long long)max_spread_relay);
	printf("  largest per-ms step in any node's global time: %lld us\n", (long long)max_step);
	if (max_spread > 100) {
		printf("  FAIL spread %lld us > 100 us\n", (long long)max_spread), failures++;
	}
	if (max_spread_relay > 1000) {
		printf("  FAIL relayed spread %lld us > 1 ms\n", (long long)max_spread_relay), failures++;
	}
	if (max_step > 100) {
		printf("  FAIL step %lld us > 100 us\n", (long long)max_step), failures++;
	}
	printf(failures ? "%d FAILURES\n" : "time sync passed\n", failures);
	return failures != 0;
}
