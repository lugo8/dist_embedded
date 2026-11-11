/*
 * PPS (Lab 5 Part 6.4): a rising edge on the PPS pin at every whole second of
 * global_gettime(), and a console line for each one. All nodes derive the edge from the
 * same global time, so the spread of the edges on a scope is the sync error.
 */
#include <zephyr/drivers/gpio.h>
#include "timesync.h"
#include "zone.h"

#define PPS_EARLY_US 300    /* wake this long before the edge, then poll */
#define PPS_WIDTH_MS 100

static const struct gpio_dt_spec pps = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), pps_gpios);

static void pps_task(void *p1, void *p2, void *p3)
{
	while (1) {
		int64_t edge = (global_gettime() / 1000000 + 1) * 1000000;
		int64_t wake = global_to_local(edge) - PPS_EARLY_US;

		k_sleep(K_TIMEOUT_ABS_US(wake));

		/* poll the last few hundred us; give up if global time moved under us */
		int64_t g, l, limit = local_us() + 2 * PPS_EARLY_US + 2000;

		while ((g = global_gettime()) < edge && (l = local_us()) < limit) {
		}
		if (g < edge || g > edge + 1000) {
			continue; /* stepped: no edge this second rather than a wrong one */
		}
		gpio_pin_set_dt(&pps, 1);
		l = local_us();

		struct timesync t;

		raft_agent_ts(&t);
		char ppm[12];

		/* Part 6.4: global and local time at the edge, drift ratio, last correction */
		agent_printf("PPS %lld: global %lld.%06lld local %lld.%06lld drift %s ppm corr %+d us%s",
			     edge / 1000000, g / 1000000, g % 1000000, l / 1000000, l % 1000000,
			     ts_ppm(ppm, t.ppb), t.last_error_us,
			     raft_agent_hb_sync_bits() & SYNC_LOCKED ? (t.locked ? "" : " (master)")
								     : " UNSYNCED");
		k_sleep(K_MSEC(PPS_WIDTH_MS));
		gpio_pin_set_dt(&pps, 0);
	}
}

K_THREAD_DEFINE(pps_tid, 1024, pps_task, NULL, NULL, NULL, PRIO_PPS, 0, SYS_FOREVER_MS);

int pps_init(void)
{
	if (gpio_pin_configure_dt(&pps, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	k_thread_start(pps_tid);
	return 0;
}
