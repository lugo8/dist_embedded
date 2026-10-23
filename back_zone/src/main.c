#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "control.h"
#include "encoder.h"
#include "io_status.h"
#include "motor.h"
#include "rx.h"
#include "tx.h"

static const struct device *const can_dev[2] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};

int main(void)
{
	printk("back zone boot\n");

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

	rc = motor_init();
	if (rc) {
		printk("motor init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
	bool encoder_available = false;
#if ENABLE_ENCODER_FEEDBACK
	encoder_available = encoder_init() == 0;
	if (!encoder_available) {
		printk("encoder unavailable; motor control will use feed-forward only\n");
	}
#else
	printk("encoder PID disabled; using feed-forward motor control\n");
#endif
	rc = blinker_init();
	if (rc) {
		printk("blinker init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
	io_status_set_blinker_ready();

	rc = rx_init();
	if (rc) {
		printk("RX init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
	control_start(encoder_available);

	rc = tx_start();
	if (rc) {
		printk("TX init failed: %d\n", rc);
		io_status_mark_error();
		return 0;
	}
	printk("back zone up\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
