#include <zephyr/sys/printk.h>
#include "zone.h"

int main(void)
{
	/* actuators first so the dispatch path never drives an unconfigured pin;
	 * monitor last so heartbeats start only once both buses are up */
	if (blinker_init() || actuator_init() || can_io_init() || monitor_init()) {
		printk("zone %d: init failed\n", MY_ZONE);
		return 0;
	}
	printk("zone %d up\n", MY_ZONE);
	return 0;
}
