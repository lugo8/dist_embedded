#include "tx.h"

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

#include "blinker.h"
#include "edf.h"
#include "io_status.h"
#include "protocol.h"

#define CAN_BUS_COUNT 2
#define HEARTBEAT_PERIOD_MS 20
#define HEARTBEAT_TX_STACK_SIZE 1024

static const struct device *const can_dev[CAN_BUS_COUNT] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};
static atomic_t tx_failed[CAN_BUS_COUNT];

static void heartbeat_tx_cb(const struct device *dev, int err, void *user_data)
{
	ARG_UNUSED(dev);

	uint8_t bus = (uint8_t)(uintptr_t)user_data;

	if (err) {
		//printk("CAN %c heartbeat transmit failed: %d\n",
		       //'A' + bus, err);
		atomic_set(&tx_failed[bus], 1);
		io_status_set_can_degraded(bus, true);
		return;
	}

	atomic_clear(&tx_failed[bus]);
	if (io_status_can_tx(bus)) {
		//printk("Failed to toggle CAN %c TX indicator\n", 'A' + bus);
	}
}

static bool bus_is_degraded(int bus)
{
	enum can_state state;
	int rc = can_get_state(can_dev[bus], &state, NULL);
	bool degraded = rc != 0 || state != CAN_STATE_ERROR_ACTIVE ||
			atomic_get(&tx_failed[bus]);

	if (rc) {
		//printk("CAN %c state check failed: %d\n", 'A' + bus, rc);
		io_status_mark_error();
	}
	static int last_deg[CAN_BUS_COUNT] = {-1, -1};

	if (last_deg[bus] != degraded) { /* TEMP debug: print on change only */
		last_deg[bus] = degraded;
		//printk("DBG bus %c degraded=%d can_state=%d(rc=%d) tx_failed=%ld\n", 'A' + bus,
		       //degraded, (int)state, rc, (long)atomic_get(&tx_failed[bus]));
	}
	if (io_status_set_can_degraded(bus, degraded)) {
		//printk("Failed to update CAN %c degraded indicator\n", 'A' + bus);
	}

	return degraded;
}

/* TEMP debug: per-step times of the last heartbeat, printed only if it was slow */
static int64_t dbg_deg_ms[CAN_BUS_COUNT];
static int64_t dbg_tx_ms[CAN_BUS_COUNT];

/* Returns true if it was queued on at least one bus that has not been failing */
static bool send_heartbeat(uint8_t seq)
{
	struct can_frame frame = {
		.id = MSG_HB_STEERING,
		.dlc = MSG_HEARTBEAT_DLC,
		.flags = 0,
	};

	bool deg[CAN_BUS_COUNT];
	bool sent = false;

	for (int bus = 0; bus < CAN_BUS_COUNT; bus++) {
		int64_t t = k_uptime_get();

		deg[bus] = bus_is_degraded(bus);
		dbg_deg_ms[bus] = k_uptime_get() - t;
	}
	encode_heartbeat(seq, deg[0], deg[1], frame.data);

	for (int bus = 0; bus < CAN_BUS_COUNT; bus++) {
		int64_t t = k_uptime_get();
		int rc = can_send(can_dev[bus], &frame, K_MSEC(5), heartbeat_tx_cb,
				  (void *)(intptr_t)bus);

		dbg_tx_ms[bus] = k_uptime_get() - t;

		if (rc) {
			//printk("can_send heartbeat on bus %c failed: %d\n", 'A' + bus, rc);
			atomic_set(&tx_failed[bus], 1);
			io_status_set_can_degraded(bus, true);
		} else if (!atomic_get(&tx_failed[bus])) {
			sent = true;
		}
	}

	return sent;
}

static void heartbeat_tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t seq = 0;
	int64_t next = k_uptime_get();

	while (1) {
		int64_t start = k_uptime_get();
		int64_t late = start - next; /* how long after its due time the thread woke */

		/* only a heartbeat that really went out may drive the shared hazard phase */
		if (send_heartbeat(seq)) {
			blinker_hazard_sync(HB_SRC_FRONT, seq);
		}
		seq++;

		int64_t took = k_uptime_get() - start;

		if (late > 5 || took > 10) { /* TEMP debug: only print the slow ones */
			//printk("HB slow seq=%u late=%d took=%d deg=%d/%d tx=%d/%d\n",
			       //(unsigned)(uint8_t)(seq - 1), (int)late, (int)took,
			       //(int)dbg_deg_ms[0], (int)dbg_deg_ms[1],
			       //(int)dbg_tx_ms[0], (int)dbg_tx_ms[1]);
		}

		next += HEARTBEAT_PERIOD_MS;
		edf_deadline_at_uptime_ms(k_current_get(),
					  next + HEARTBEAT_PERIOD_MS,
					  EDF_TIE_HEARTBEAT);
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(heartbeat_tx_tid, HEARTBEAT_TX_STACK_SIZE, heartbeat_tx_thread,
		NULL, NULL, NULL, EDF_THREAD_PRIORITY, 0, SYS_FOREVER_MS);

int tx_start(void)
{
	for (int bus = 0; bus < CAN_BUS_COUNT; bus++) {
		if (!device_is_ready(can_dev[bus])) {
			printk("CAN %c not ready; heartbeat TX not started\n", 'A' + bus);
			io_status_mark_error();
			return -ENODEV;
		}

		enum can_state state;
		int rc = can_get_state(can_dev[bus], &state, NULL);
		if (rc) {
			printk("CAN %c state check failed: %d\n", 'A' + bus, rc);
			io_status_mark_error();
			return rc;
		}
		if (state == CAN_STATE_STOPPED) {
			printk("CAN %c is stopped; heartbeat TX not started\n", 'A' + bus);
			io_status_mark_error();
			return -EIO;
		}
	}

	edf_deadline_after_ms(heartbeat_tx_tid, HEARTBEAT_PERIOD_MS,
			      EDF_TIE_HEARTBEAT);
	k_thread_start(heartbeat_tx_tid);
	return 0;
}
