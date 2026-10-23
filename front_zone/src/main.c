#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
//#include "control.h"
//#include "current.h"
//#include "inject.h"
#include "io_status.h"
#include "rx.h"
#include "servo.h"
#include "tx.h"

/* 1: keyboard injector over the PA9<->PA10 jumper, 0: real Pi on usart1 */
#define USE_INJECTOR 0

// static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

static const struct device *const can_dev[2] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};

int main(void)
{
	printk("front zone boot\n");

	int rc = io_status_init();
	if (rc) {
		printk("GPIO indicator init failed: %d\n", rc);
		return 0;
	}

	if (!device_is_ready(can_dev[0])) {
		printk("CAN A not ready\n");
		io_status_mark_error();
		return 0;
	}

	if (!device_is_ready(can_dev[1])) {
		printk("CAN B not ready\n");
		io_status_mark_error();
		return 0;
	}

	/* current zero is measured inside current_init(), so the motors must be
	 * coasting and the servo undriven until it returns */
	if (servo_init()) {
		printk("peripheral init failed\n");
		io_status_mark_error();
		return 0;
	}
	rc = blinker_init();
	if (rc) {
		printk("blinker init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
	rc = rx_init();
	if (rc) {
		printk("RX init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}

	rc = tx_start();
	if (rc) {
		printk("TX init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
#if USE_INJECTOR
	inject_start(); /* TEMP: stands in for the Pi, needs PA9<->PA10 jumper */
#endif

	printk("front zone up\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
