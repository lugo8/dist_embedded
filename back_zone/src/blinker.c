#include "blinker.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "io_status.h"
#include "edf.h"
#include "protocol.h"

#define ERROR_BLINK_ON_MS 500 /* R2.4: 1 Hz +-10 % */
#define ERROR_BLINK_OFF_MS 500
#define BLINK_TICK_MS 10
/* No-RPi hazard blink: phase-locked to the front zone's 20 ms heartbeat seq.
 * The 8-bit seq is cut into 10 half periods of 25.6 beats (25 or 26 beats,
 * 500 or 520 ms, ~0.98 Hz). 10 is even, so the phase also carries across the
 * seq wrap with no long half period. Both zones flip when that heartbeat is
 * sent / received, so the edges match to well under 1 ms.
 */
#define HAZARD_HALVES_PER_WRAP 10
#define HAZARD_SYNC_STALE_MS 100 /* 5 missed heartbeats: free-run instead */
#define HAZARD_RETURN_STEADY_MS 200 /* front must beat this long, unbroken, before leaving the back's seq for it */
#define BLINK_THREAD_STACK_SIZE 1024
/* The RPi re-sends BLINKER every 20 ms; this many ms without one means it is gone. */
#define REMOTE_STALE_MS 100

static const struct gpio_dt_spec rl = GPIO_DT_SPEC_GET(DT_NODELABEL(rl), gpios);
static const struct gpio_dt_spec rr = GPIO_DT_SPEC_GET(DT_NODELABEL(rr), gpios);

static atomic_t remote_led_mask;
static atomic_t remote_last_ms; /* when the last valid BLINKER frame arrived */
static atomic_t remote_seen;
static K_MUTEX_DEFINE(led_mutex);
static atomic_t hazard_phase_on[HB_SRC_COUNT];
static atomic_t hazard_sync_valid[HB_SRC_COUNT];
static atomic_t hazard_sync_ms[HB_SRC_COUNT];
static atomic_t hazard_fresh_since[HB_SRC_COUNT]; /* start of the current unbroken run of heartbeats */
static int hazard_ref = HB_SRC_FRONT; /* heartbeat the pattern is locked to, only touched under led_mutex */
/* free-run fallback state, only touched under led_mutex */
static bool free_run_on;
static int64_t free_run_next_ms;

static int set_leds(bool left, bool right)
{
	int rc = gpio_pin_set_dt(&rl, left);

	if (rc) {
		io_status_mark_error();
		printk("Rear-left blinker GPIO failed: %d\n", rc);
		return rc;
	}
	rc = gpio_pin_set_dt(&rr, right);
	if (rc) {
		io_status_mark_error();
		printk("Rear-right blinker GPIO failed: %d\n", rc);
	}

	return rc;
}

/* Called from the BLINKER rx handler and the blink thread; the lock stops
 * the thread from writing a stale mask over a fresh one.
 */
static void apply_remote_mask(void)
{
	k_mutex_lock(&led_mutex, K_FOREVER);
	uint8_t led_mask = atomic_get(&remote_led_mask);

	(void)set_leds((led_mask & BIT(0)) != 0, (led_mask & BIT(1)) != 0);
	k_mutex_unlock(&led_mutex);
}

static bool hb_fresh(int src)
{
	return atomic_get(&hazard_sync_valid[src]) &&
	       (uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&hazard_sync_ms[src])) <
		       HAZARD_SYNC_STALE_MS;
}

static bool hb_steady(int src)
{
	return hb_fresh(src) &&
	       (uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&hazard_fresh_since[src])) >=
		       HAZARD_RETURN_STEADY_MS;
}

/* Front is preferred, the back is the fallback. Sticky: once on the back, go back to the front only when it
 * has been steady, so a flapping front heartbeat does not bounce the phase. Call with led_mutex held.
 * Returns the heartbeat to lock to, or -1 if neither is alive.
 */
static int hazard_pick_ref(void)
{
	if (hazard_ref == HB_SRC_BACK) {
		if (hb_steady(HB_SRC_FRONT) || (!hb_fresh(HB_SRC_BACK) && hb_fresh(HB_SRC_FRONT))) {
			hazard_ref = HB_SRC_FRONT;
		}
	} else if (!hb_fresh(HB_SRC_FRONT) && hb_fresh(HB_SRC_BACK)) {
		hazard_ref = HB_SRC_BACK;
	}

	return hb_fresh(hazard_ref) ? hazard_ref : -1;
}

/* Write the no-RPi hazard pattern. Locked so the thread cannot overwrite an
 * edge with a phase it read just before it.
 */
static void apply_hazard_leds(void)
{
	k_mutex_lock(&led_mutex, K_FOREVER);
	bool on;

	int ref = hazard_pick_ref();

	if (ref >= 0) {
		on = atomic_get(&hazard_phase_on[ref]);
	} else { /* both heartbeats gone: free-run */
		int64_t now = k_uptime_get();

		if (now >= free_run_next_ms) {
			free_run_on = !free_run_on;
			free_run_next_ms = now + (free_run_on ? ERROR_BLINK_ON_MS :
								ERROR_BLINK_OFF_MS);
		}
		on = free_run_on;
	}
	(void)set_leds(on, on);
	k_mutex_unlock(&led_mutex);
}

void blinker_hazard_sync(enum hb_src src, uint8_t hb_seq)
{
	bool on = ((hb_seq * HAZARD_HALVES_PER_WRAP) >> 8) & 1;
	uint32_t now = k_uptime_get_32();

	if (!hb_fresh(src)) { /* a gap before this beat: a new unbroken run starts */
		atomic_set(&hazard_fresh_since[src], now);
	}
	atomic_set(&hazard_sync_ms[src], now);
	atomic_set(&hazard_sync_valid[src], 1);
	/* a missed edge frame is fixed by the next heartbeat, 20 ms later */
	if (atomic_set(&hazard_phase_on[src], on) != on && src == hazard_ref &&
	    io_status_blinker_hazard_override()) {
		apply_hazard_leds();
	}
}

static void blinker_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	static int dbg_override = -1; /* TEMP debug: print on change only */

	while (1) {
		int ovr = io_status_blinker_hazard_override();

		if (ovr != dbg_override) {
			dbg_override = ovr;
			printk("BACK BLK override=%d ref=%d front=%d back=%d t=%u\n", ovr, hazard_ref,
			       hb_fresh(HB_SRC_FRONT), hb_fresh(HB_SRC_BACK), k_uptime_get_32());
		}
		if (ovr) {
			apply_hazard_leds();
		} else {
			apply_remote_mask();
		}
		int64_t next = k_uptime_get() + BLINK_TICK_MS;

		edf_deadline_at_uptime_ms(k_current_get(),
					  next + BLINK_TICK_MS,
					  EDF_TIE_BLINKER);
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(blinker_tid, BLINK_THREAD_STACK_SIZE, blinker_thread,
		NULL, NULL, NULL, EDF_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

int blinker_init(void)
{
	if (!gpio_is_ready_dt(&rl) || !gpio_is_ready_dt(&rr)) {
		return -ENODEV;
	}

	int rc = gpio_pin_configure_dt(&rl, GPIO_OUTPUT_INACTIVE);

	if (rc) {
		return rc;
	}

	rc = gpio_pin_configure_dt(&rr, GPIO_OUTPUT_INACTIVE);
	if (rc) {
		return rc;
	}

	edf_deadline_after_ms(blinker_tid, BLINK_TICK_MS, EDF_TIE_BLINKER);
	k_thread_start(blinker_tid);
	return 0;
}

int blinker_set_message(uint8_t new_state, bool leds_on)
{
	if (new_state > BLINKERS_HAZARD) {
		return -EINVAL;
	}

	uint8_t led_mask = 0;

	switch (new_state) {
	case BLINKERS_OFF:
		led_mask = 0;
		break;
	case BLINKERS_LEFT:
		led_mask = leds_on ? BIT(0) : 0;
		break;
	case BLINKERS_RIGHT:
		led_mask = leds_on ? BIT(1) : 0;
		break;
	case BLINKERS_HAZARD:
		led_mask = leds_on ? (BIT(0) | BIT(1)) : 0;
		break;
	}

	atomic_set(&remote_led_mask, led_mask);
	/* set before the override check below, so a fresh BLINKER switches this zone to it at once */
	atomic_set(&remote_last_ms, k_uptime_get_32());
	atomic_set(&remote_seen, 1);
	if (!io_status_blinker_hazard_override()) {
		apply_remote_mask(); /* edge shows now, not on the next 10 ms tick */
	}
	return 0;
}

bool blinker_remote_fresh(void)
{
	return atomic_get(&remote_seen) &&
	       (uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&remote_last_ms)) < REMOTE_STALE_MS;
}
