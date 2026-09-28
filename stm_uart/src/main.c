#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "rx.h"

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

int main(void)
{
	printk("START MAIN\n");
	if (!device_is_ready(pi_uart)) {
		printk("usart1 (Pi link) not ready\n");
		return 0;
	}

	printk("DEVICE DETECTED\n");

	rx_init();

	printk("car_fw up, usart1 ready\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
