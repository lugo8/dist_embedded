/*
 * BLINK_F / BLINK_R on global time (Lab 5 Part 6).
 *
 * Lab 3: the lamps switched when the cockpit's BLINK frame arrived, so front and rear were
 * in step only while the Pi was there. Lab 5: the lamp state is a function of global time,
 * the same on every node, so they stay in step with no cockpit at all:
 *   - lamps change only on a 50 ms grid of global time;
 *   - hazards are on in the first half of each period counted from global time 0:
 *     2 Hz in the error state, 4 Hz in failure mode;
 *   - a turn signal counts its 1 Hz from the grid point the Pi picked for its start
 *     (CTRL b7 bits 2-7, 10-60 ms after the button, R2.4). Picking it on each STM32 from
 *     when CTRL arrived was not enough: a relayed copy arrives ~1 ms later, and just
 *     before a grid point that is a different grid point, 50 ms out of step;
 *   - failure mode is taken up only on a 250 ms boundary, where the 2 Hz and 4 Hz
 *     patterns both have an edge: every node learns it from the same REF, so every node
 *     switches on the same edge.
 */
#include <zephyr/drivers/gpio.h>
#include "zone.h"

#define GRID_US (BLINK_GRID_MS * 1000LL)
#define EARLY_US 200

static const struct gpio_dt_spec led_l = GPIO_DT_SPEC_GET(DT_NODELABEL(blink_left), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(blink_right), gpios);

static atomic_t ctrl_state = ATOMIC_INIT(BLINK_OFF);
static atomic_t ctrl_slot;
static atomic_t ctrl_ms; /* k_uptime of the last CTRL, 0 = never */

/* called from dispatch for every valid CTRL */
void blinker_ctrl(uint8_t state, uint8_t slot)
{
	atomic_set(&ctrl_slot, slot);
	atomic_val_t old = atomic_set(&ctrl_state, state);

	atomic_set(&ctrl_ms, (atomic_val_t)k_uptime_get_32() | 1);
#ifdef ZONE_FRONT
	/* N2 logs the blinker on/off events (one node, so each change is logged once) */
	if (old != state) {
		static const uint8_t kind[] = { APP_BLINK_OFF, APP_LEFT_ON, APP_RIGHT_ON,
						APP_HAZARD_ON };

		raft_agent_event(kind[state & 3]);
	}
#else
	ARG_UNUSED(old);
#endif
}

static bool phase_on(int64_t g, int64_t anchor, int half_ms)
{
	return g >= anchor && ((g - anchor) / (half_ms * 1000LL)) % 2 == 0;
}

/* the grid point nearest `edge` (within +/- 1.6 s) whose slot number is `slot` */
static int64_t slot_time(int64_t edge, uint8_t slot)
{
	int64_t k = edge / TURN_GRID_US;
	int delta = (int)((slot - k) & 0x3F);

	if (delta > 32) {
		delta -= 64;
	}
	return (k + delta) * TURN_GRID_US;
}

static void blink_task(void *p1, void *p2, void *p3)
{
	bool failure = false;
	uint8_t turn = BLINK_OFF, turn_slot = 0;
	int64_t turn_anchor = 0;
	int shown = -1;
	int64_t shown_anchor = -1;

	while (1) {
		int64_t edge = (global_gettime() / GRID_US + 1) * GRID_US;

		k_sleep(K_TIMEOUT_ABS_US(global_to_local(edge) - EARLY_US));
		int64_t limit = local_us() + 2 * EARLY_US + 2000;

		while (global_gettime() < edge && local_us() < limit) {
		}

		if (edge % (HAZARD_HALF_MS * 1000LL) == 0) {
			failure = raft_agent_failure_mode();
		}
		uint32_t seen = (uint32_t)atomic_get(&ctrl_ms);
		bool fresh = seen && k_uptime_get_32() - seen < BLINK_STALE_MS;
		uint8_t state = fresh ? (uint8_t)atomic_get(&ctrl_state) : BLINK_OFF;
		uint8_t slot = (uint8_t)atomic_get(&ctrl_slot);
		bool l, r;

		if (state == BLINK_LEFT || state == BLINK_RIGHT) {
			if (turn != state || turn_slot != slot) {
				/* latch it once: slot numbers repeat every 3.2 s */
				turn = state;
				turn_slot = slot;
				turn_anchor = slot_time(edge, slot); /* the 1 Hz starts there, lamps on */
			}
		} else {
			turn = BLINK_OFF;
		}

		if (failure) {
			l = r = phase_on(edge, 0, FAILURE_HALF_MS);
		} else if (monitor_in_error() || state == BLINK_HAZARD) {
			l = r = phase_on(edge, 0, HAZARD_HALF_MS);
		} else if (turn != BLINK_OFF) {
			bool on = phase_on(edge, turn_anchor, BLINK_HALF_MS);

			l = on && turn == BLINK_LEFT;
			r = on && turn == BLINK_RIGHT;
		} else {
			l = r = false;
		}
		gpio_pin_set_dt(&led_l, l);
		gpio_pin_set_dt(&led_r, r);

		/* say what the lamps are following, on every change: compare N2's and N3's lines */
		int mode = failure ? 4 : (monitor_in_error() || state == BLINK_HAZARD) ? 3 : turn;

		if (RAFT_CONF_DEBUG &&
		    (mode != shown || (turn != BLINK_OFF && turn_anchor != shown_anchor))) {
			static const char *const name[] = { "off", "LEFT 1 Hz", "RIGHT 1 Hz",
							    "hazard 2 Hz", "failure 4 Hz" };

			agent_printf("[%lld] blink: %s, global %lld.%06lld, turn start %lld.%06lld",
				     k_uptime_get(), name[mode], edge / 1000000, edge % 1000000,
				     turn_anchor / 1000000, turn_anchor % 1000000);
			shown = mode;
			shown_anchor = turn_anchor;
		}
	}
}

K_THREAD_DEFINE(blink_tid, 1024, blink_task, NULL, NULL, NULL, PRIO_ACT, 0, SYS_FOREVER_MS);

int blinker_init(void)
{
	if (!gpio_is_ready_dt(&led_l) || !gpio_is_ready_dt(&led_r) ||
	    gpio_pin_configure_dt(&led_l, GPIO_OUTPUT_INACTIVE) ||
	    gpio_pin_configure_dt(&led_r, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	k_thread_start(blink_tid);
	return 0;
}
