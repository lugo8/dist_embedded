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

#define BLINK_PERIOD_MS 500
#define ERROR_BLINK_ON_MS 1000
#define ERROR_BLINK_OFF_MS 1000
#define BLINK_THREAD_STACK_SIZE 768
#define BLINK_THREAD_PRIORITY 5

static const struct gpio_dt_spec rl = GPIO_DT_SPEC_GET(DT_NODELABEL(rl), gpios);
static const struct gpio_dt_spec rr = GPIO_DT_SPEC_GET(DT_NODELABEL(rr), gpios);

static atomic_t remote_led_mask;
static atomic_t error_mode = ATOMIC_INIT(0);

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

static void blinker_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	bool error_leds_on = false;

	while (1) {
		bool in_error = atomic_get(&error_mode);

		if (in_error) {
			error_leds_on = !error_leds_on;
			(void)set_leds(error_leds_on, error_leds_on);
			k_sleep(K_MSEC(error_leds_on ? ERROR_BLINK_ON_MS :
						       ERROR_BLINK_OFF_MS));
		} else {
			error_leds_on = false;
			uint8_t led_mask = atomic_get(&remote_led_mask);

			(void)set_leds((led_mask & BIT(0)) != 0,
				       (led_mask & BIT(1)) != 0);
			k_sleep(K_MSEC(BLINK_PERIOD_MS));
		}
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

void blinker_set_error_mode(bool enabled)
{
	if (enabled) {
		atomic_set(&error_mode, 1);
	} else {
		atomic_set(&error_mode, 0);
		atomic_set(&remote_led_mask, 0);
		(void)set_leds(false, false);
	}
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
	return 0;
}
