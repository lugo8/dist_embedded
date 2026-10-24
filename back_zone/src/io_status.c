#include "io_status.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "motor.h"
#include "protocol.h"
#include "edf.h"
#include "state.h"

#define LINK_WATCHDOG_PERIOD_MS 10
#define LINK_WATCHDOG_STACK_SIZE 512

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

static void link_watchdog_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int64_t next = k_uptime_get();

	while (1) {
		if (state_zone_control_missing_on_both_buses() &&
		    state_get_zone_state() != ZONE_STATE_FAILSAFE_LINK_LOST) {
			io_status_enter_error(ZONE_STATE_FAILSAFE_LINK_LOST);
		}

		next += LINK_WATCHDOG_PERIOD_MS;
		edf_deadline_at_uptime_ms(k_current_get(),
					  next + LINK_WATCHDOG_PERIOD_MS,
					  EDF_TIE_CAN_RX);
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(link_watchdog_tid, LINK_WATCHDOG_STACK_SIZE,
		link_watchdog_thread, NULL, NULL, NULL,
		EDF_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

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
	edf_deadline_after_ms(link_watchdog_tid, LINK_WATCHDOG_PERIOD_MS,
			      EDF_TIE_CAN_RX);
	k_thread_start(link_watchdog_tid);
	return 0;
}

void io_status_enter_error(uint8_t zone_state)
{
	static int dbg_last = -1; /* TEMP debug: print on change only */

	if (dbg_last != zone_state) {
		dbg_last = zone_state;
		//printk("DBG enter_error: %s caller=%p\n", zone_state_name(zone_state),
		       //__builtin_return_address(0));
	}
	atomic_set(&error_active, 1);
	state_set_zone_state(zone_state);
	(void)gpio_pin_set_dt(&error_pin, 1);
	int rc = motor_stop();

	if (rc) {
		//printk("Motor failsafe stop failed: %d\n", rc);
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
}

bool io_status_error_active(void)
{
	return atomic_get(&error_active) != 0;
}

void io_status_note_control_received(void)
{
	io_status_clear_error();
}

void io_status_mark_error(void)
{
	//printk("DBG mark_error caller=%p\n", __builtin_return_address(0));
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

bool io_status_is_can_degraded(uint8_t bus)
{
	return bus >= ARRAY_SIZE(can_degraded) ||
	       (atomic_get(&degraded_mask) & BIT(bus)) != 0;
}

/* Hazard phase source:
 *  - RPi alive (zone-control messages on either bus): follow its BLINKER messages, so every zone shows its phase
 *  - RPi dead: show the pattern locked to the front zone's heartbeat seq (blinker_hazard_sync), so the
 *    zones agree with each other. Free-runs if that heartbeat is gone too.
 * Our own CAN degradation is deliberately not a condition: it must not make this zone leave the RPi's phase.
 */
bool io_status_blinker_hazard_override(void)
{
	return state_zone_control_missing_on_both_buses();
}
