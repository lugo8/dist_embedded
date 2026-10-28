/* BLINK_F / BLINK_R: follow the cockpit's BLINK frames; blink hazards locally in error */
#include <zephyr/drivers/gpio.h>
#include "zone.h"

static const struct gpio_dt_spec led_l = GPIO_DT_SPEC_GET(DT_NODELABEL(blink_left), gpios);
static const struct gpio_dt_spec led_r = GPIO_DT_SPEC_GET(DT_NODELABEL(blink_right), gpios);

static K_SEM_DEFINE(blink_sem, 0, 1);
static K_MUTEX_DEFINE(blink_lock);
static uint8_t msg_state = BLINK_OFF;
static uint8_t msg_phase;
static int64_t msg_ms; /* 0 = never */

/* called from dispatch; the LEDs change on reception, which is what keeps front and rear in sync */
void blinker_msg(uint8_t state, uint8_t phase)
{
	if (state > BLINK_HAZARD) {
		return;
	}
	k_mutex_lock(&blink_lock, K_FOREVER);
	msg_state = state;
	msg_phase = phase & 1;
	msg_ms = k_uptime_get();
	k_mutex_unlock(&blink_lock);
	k_sem_give(&blink_sem);
}

static void blink_task(void *p1, void *p2, void *p3)
{
	bool local_on = false;
	int64_t local_next = 0;

	while (1) {
		/* wake on a BLINK frame, or every 10 ms for the local hazard pattern */
		k_sem_take(&blink_sem, K_MSEC(10));
		int64_t now = k_uptime_get();

		k_mutex_lock(&blink_lock, K_FOREVER);
		uint8_t state = msg_state;
		bool on = msg_phase;
		bool fresh = msg_ms > 0 && now - msg_ms < BLINK_STALE_MS;
		k_mutex_unlock(&blink_lock);

		bool l, r;

		if (monitor_in_error() && !(fresh && state == BLINK_HAZARD)) {
			/* our own error without the cockpit's hazard pattern to follow */
			if (now >= local_next) {
				local_on = !local_on;
				local_next = now + HAZARD_HALF_MS;
			}
			l = r = local_on;
		} else {
			l = on && (state == BLINK_LEFT || state == BLINK_HAZARD);
			r = on && (state == BLINK_RIGHT || state == BLINK_HAZARD);
		}
		gpio_pin_set_dt(&led_l, l);
		gpio_pin_set_dt(&led_r, r);
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
