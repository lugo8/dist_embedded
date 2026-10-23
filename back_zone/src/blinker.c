#include "blinker.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "io_status.h"
#include "protocol.h"

#define ERROR_BLINK_ON_MS 500 /* R2.4: 1 Hz +-10 % */
#define ERROR_BLINK_OFF_MS 500
#define BLINK_TICK_MS 10
/* No-RPi hazard blink: phase-locked to the front zone's 20 ms heartbeat seq.
 * 25 beats = 500 ms per half period (1 Hz). Both zones derive the phase from
 * the same seq and flip when that heartbeat is sent / received, so the edges
 * match to well under 1 ms. 256 is not a multiple of 25, so once per wrap
 * (5.12 s) one half period is longer; both zones still agree.
 */
#define HAZARD_BEATS_PER_HALF 25
#define HAZARD_SYNC_STALE_MS 100 /* 5 missed heartbeats: free-run instead */
#define BLINK_THREAD_STACK_SIZE 768
#define BLINK_THREAD_PRIORITY 5

static const struct gpio_dt_spec rl = GPIO_DT_SPEC_GET(DT_NODELABEL(rl), gpios);
static const struct gpio_dt_spec rr = GPIO_DT_SPEC_GET(DT_NODELABEL(rr), gpios);

static atomic_t remote_led_mask;
static K_MUTEX_DEFINE(led_mutex);
static atomic_t hazard_phase_on;
static atomic_t hazard_sync_valid;
static atomic_t hazard_sync_ms;
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

static bool hazard_synced(void)
{
	return atomic_get(&hazard_sync_valid) &&
	       (uint32_t)(k_uptime_get_32() - (uint32_t)atomic_get(&hazard_sync_ms)) <
		       HAZARD_SYNC_STALE_MS;
}

/* Write the no-RPi hazard pattern. Locked so the thread cannot overwrite an
 * edge with a phase it read just before it.
 */
static void apply_hazard_leds(void)
{
	k_mutex_lock(&led_mutex, K_FOREVER);
	bool on;

	if (hazard_synced()) {
		on = atomic_get(&hazard_phase_on);
	} else { /* partner HB gone: free-run */
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

void blinker_hazard_sync(uint8_t hb_seq)
{
	bool on = (hb_seq / HAZARD_BEATS_PER_HALF) & 1;

	atomic_set(&hazard_sync_ms, k_uptime_get_32());
	atomic_set(&hazard_sync_valid, 1);
	/* a missed edge frame is fixed by the next heartbeat, 20 ms later */
	if (atomic_set(&hazard_phase_on, on) != on &&
	    io_status_blinker_hazard_override()) {
		apply_hazard_leds();
	}
}

static void blinker_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (1) {
		if (io_status_blinker_hazard_override()) {
			apply_hazard_leds();
		} else {
			apply_remote_mask();
		}
		k_sleep(K_MSEC(BLINK_TICK_MS));
	}
}

K_THREAD_DEFINE(blinker_tid, BLINK_THREAD_STACK_SIZE, blinker_thread,
		NULL, NULL, NULL, BLINK_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

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
	if (!io_status_blinker_hazard_override()) {
		apply_remote_mask(); /* edge shows now, not on the next 10 ms tick */
	}
	return 0;
}
