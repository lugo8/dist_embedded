#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "control.h"
#include "current.h"
#include "encoder.h"
#include "inject.h"
#include "motor.h"
#include "rx.h"
#include "servo.h"
#include "tx.h"

/* 1: keyboard injector over the PA9<->PA10 jumper, 0: real Pi on usart1 */
#define USE_INJECTOR 0

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

int main(void)
{
	printk("zone_1 boot\n");

	if (!device_is_ready(pi_uart)) {
		printk("usart1 (Pi link) not ready\n");
		return 0;
	}

	/* current zero is measured inside current_init(), so the motors must be
	 * coasting and the servo undriven until it returns */
	if (encoder_init() || motor_init() || current_init() || servo_init()) {
		printk("peripheral init failed\n");
		return 0;
	}
	blinker_init();

	rx_init();
	control_start();
	tx_start();
#if USE_INJECTOR
	inject_start(); /* TEMP: stands in for the Pi, needs PA9<->PA10 jumper */
#endif

	printk("zone_1 up\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
