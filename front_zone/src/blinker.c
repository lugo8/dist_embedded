#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include "blinker.h"

/* wheel angle 0-900 deg, lower = left */
#define LEFT_TURN_THRESHOLD 299
#define RIGHT_TURN_THRESHOLD 601
#define TICK_MS 250

enum { OFF, LEFT, RIGHT, HAZARD };

static const struct gpio_dt_spec fl = GPIO_DT_SPEC_GET(DT_NODELABEL(fl), gpios);
static const struct gpio_dt_spec fr = GPIO_DT_SPEC_GET(DT_NODELABEL(fr), gpios);
static const struct gpio_dt_spec rl = GPIO_DT_SPEC_GET(DT_NODELABEL(rl), gpios);
static const struct gpio_dt_spec rr = GPIO_DT_SPEC_GET(DT_NODELABEL(rr), gpios);

/* written by threads, read in the timer isr */
static atomic_t state = ATOMIC_INIT(OFF);
static bool passed;

static void tick(struct k_timer *t)
{
	static int last = OFF;
	static uint32_t n;
	int s = atomic_get(&state);

	/* restart the pattern on a change so it starts in the on phase */
	if (s != last) {
		n = 0;
		last = s;
	}

	bool slow = ((n >> 1) & 1) == 0;  /* 500 ms on, 500 ms off */
	bool fast = (n & 1) == 0;         /* 250 ms on, 250 ms off */
	n++;

	bool l = (s == LEFT && slow) || (s == HAZARD && fast);
	bool r = (s == RIGHT && slow) || (s == HAZARD && fast);

	/* same callback for front and rear keeps each side in sync */
	gpio_pin_set_dt(&fl, l);
	gpio_pin_set_dt(&rl, l);
	gpio_pin_set_dt(&fr, r);
	gpio_pin_set_dt(&rr, r);
}

K_TIMER_DEFINE(blink_timer, tick, NULL);

static void set_state(int s)
{
	if (atomic_set(&state, s) != s) {
		passed = false;
		/* fire now instead of waiting up to a full tick */
		k_timer_start(&blink_timer, K_NO_WAIT, K_MSEC(TICK_MS));
	}
}

void blinker_init(void)
{
	gpio_pin_configure_dt(&fl, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&fr, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rl, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&rr, GPIO_OUTPUT_INACTIVE);
	k_timer_start(&blink_timer, K_NO_WAIT, K_MSEC(TICK_MS));
}

/* hazards win over turn signals until they are cleared */
void blinker_left(void)
{
	if (atomic_get(&state) != HAZARD) {
		set_state(LEFT);
	}
}

void blinker_right(void)
{
	if (atomic_get(&state) != HAZARD) {
		set_state(RIGHT);
	}
}

void blinker_hazard(bool on)
{
	if (on) {
		set_state(HAZARD);
	} else if (atomic_get(&state) == HAZARD) {
		set_state(OFF);
	}
}

/* cancel once the wheel goes past the threshold and comes back */
void blinker_steer(int steer)
{
	int s = atomic_get(&state);

	if (s == LEFT) {
		if (steer < LEFT_TURN_THRESHOLD) {
			passed = true;
		} else if (passed) {
			set_state(OFF);
		}
	} else if (s == RIGHT) {
		if (steer > RIGHT_TURN_THRESHOLD) {
			passed = true;
		} else if (passed) {
			set_state(OFF);
		}
	}
}