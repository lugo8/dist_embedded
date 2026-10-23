#include "io_status.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "blinker.h"
#include "motor.h"
#include "protocol.h"
#include "state.h"

static const struct gpio_dt_spec can_degraded[2] = {
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_a_degraded_gpios),
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_b_degraded_gpios),
};
static const struct gpio_dt_spec can_rx[2] = {
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_a_rx_gpios),
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_b_rx_gpios),
};
static const struct gpio_dt_spec can_tx[2] = {
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_a_tx_gpios),
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), can_b_tx_gpios),
};
static const struct gpio_dt_spec error_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_err_gpios);

static atomic_t degraded_mask;
static atomic_t error_active;
static atomic_t blinker_ready;

static int configure_output(const struct gpio_dt_spec *pin)
{
	if (!gpio_is_ready_dt(pin)) {
		return -ENODEV;
	}

	return gpio_pin_configure_dt(pin, GPIO_OUTPUT_INACTIVE);
}

int io_status_init(void)
{
	atomic_clear(&degraded_mask);
	atomic_clear(&error_active);
	atomic_clear(&blinker_ready);

	int rc = configure_output(&error_pin);

	if (rc) {
		return rc;
	}

	for (size_t i = 0; i < ARRAY_SIZE(can_degraded); i++) {
		rc = configure_output(&can_degraded[i]);

		if (rc) {
			io_status_mark_error();
			return rc;
		}
		rc = configure_output(&can_rx[i]);
		if (rc) {
			io_status_mark_error();
			return rc;
		}
		rc = configure_output(&can_tx[i]);
		if (rc) {
			io_status_mark_error();
			return rc;
		}
	}

	io_status_enter_error(ZONE_STATE_FAILSAFE_LINK_LOST);
	return 0;
}

void io_status_enter_error(uint8_t zone_state)
{
	atomic_set(&error_active, 1);
	state_set_zone_state(zone_state);
	(void)gpio_pin_set_dt(&error_pin, 1);
	int rc = motor_stop();

	if (rc) {
		printk("Motor failsafe stop failed: %d\n", rc);
	}
	if (atomic_get(&blinker_ready)) {
		blinker_set_error_mode(true);
	}
}

void io_status_clear_error(void)
{
	atomic_clear(&error_active);
	state_set_zone_state(ZONE_STATE_NORMAL);
	int rc = gpio_pin_set_dt(&error_pin, 0);

	if (rc) {
		io_status_enter_error(ZONE_STATE_FAILSAFE_RPI_ERROR);
		return;
	}
	if (atomic_get(&blinker_ready)) {
		blinker_set_error_mode(false);
	}
}

bool io_status_error_active(void)
{
	return atomic_get(&error_active) != 0;
}

void io_status_note_control_received(void)
{
	if (state_get_zone_state() == ZONE_STATE_FAILSAFE_LINK_LOST) {
		io_status_clear_error();
	}
}

void io_status_mark_error(void)
{
	io_status_enter_error(ZONE_STATE_FAILSAFE_RPI_ERROR);
}

static int toggle_pin(const struct gpio_dt_spec *pin)
{
	int rc = gpio_pin_toggle_dt(pin);

	if (rc) {
		io_status_mark_error();
	}

	return rc;
}

int io_status_can_rx(uint8_t bus)
{
	if (bus >= ARRAY_SIZE(can_rx)) {
		io_status_mark_error();
		return -EINVAL;
	}

	return toggle_pin(&can_rx[bus]);
}

int io_status_can_tx(uint8_t bus)
{
	if (bus >= ARRAY_SIZE(can_tx)) {
		io_status_mark_error();
		return -EINVAL;
	}

	return toggle_pin(&can_tx[bus]);
}

int io_status_set_can_degraded(uint8_t bus, bool degraded)
{
	if (bus >= ARRAY_SIZE(can_degraded)) {
		io_status_mark_error();
		return -EINVAL;
	}

	atomic_val_t bit = BIT(bus);

	if (degraded) {
		atomic_or(&degraded_mask, bit);
	} else {
		atomic_and(&degraded_mask, ~bit);
	}

	int rc = gpio_pin_set_dt(&can_degraded[bus], degraded);

	if (rc) {
		io_status_enter_error(ZONE_STATE_FAILSAFE_RPI_ERROR);
		return rc;
	}
	atomic_val_t degraded_flags = atomic_get(&degraded_mask);

	if (degraded_flags == (BIT(0) | BIT(1))) {
		io_status_enter_error(ZONE_STATE_FAILSAFE_LINK_LOST);
	} else if (state_get_zone_state() == ZONE_STATE_FAILSAFE_LINK_LOST) {
		io_status_clear_error();
	}

	return rc;
}

void io_status_set_blinker_ready(void)
{
	atomic_set(&blinker_ready, 1);
	blinker_set_error_mode(io_status_error_active());
}
