#include <zephyr/sys/printk.h>
#include "zone.h"

int main(void)
{
	/* actuators first so the dispatch path never drives an unconfigured pin; the raft
	 * agent before CAN so it can take frames; monitor so heartbeats start only once both
	 * buses are up; PPS last */
	if (blinker_init() || actuator_init() || raft_agent_init() || can_io_init() ||
	    monitor_init() || pps_init()) {
		printk("zone %d: init failed\n", MY_ZONE);
		return 0;
	}
	printk("zone %d up\n", MY_ZONE);
	return 0;
}
