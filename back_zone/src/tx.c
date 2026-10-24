#include "tx.h"

#include <errno.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/atomic.h>

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

static void send_heartbeat(uint8_t seq)
{
	struct can_frame frame = {
		.id = MSG_HB_DRIVETRAIN,
		.dlc = MSG_HB_DRIVETRAIN_DLC,
		.flags = 0,
	};
	const uint16_t dummy_force_feedback = 0;

	encode_heartbeat(seq, bus_is_degraded(0), bus_is_degraded(1), frame.data);
	frame.data[HB_DRIVETRAIN_FORCE_MSB] =
		(uint8_t)(dummy_force_feedback >> 8);
	frame.data[HB_DRIVETRAIN_FORCE_LSB] = (uint8_t)dummy_force_feedback;
	frame.data[HB_DRIVETRAIN_SEQ] = seq;

	for (int bus = 0; bus < CAN_BUS_COUNT; bus++) {
		int rc = can_send(can_dev[bus], &frame, K_MSEC(5), heartbeat_tx_cb,
				  (void *)(intptr_t)bus);

		if (rc) {
			//printk("can_send heartbeat on bus %c failed: %d\n", 'A' + bus, rc);
			atomic_set(&tx_failed[bus], 1);
			io_status_set_can_degraded(bus, true);
		}
	}
}

static void heartbeat_tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t seq = 0;
	int64_t next = k_uptime_get();

	while (1) {
		send_heartbeat(seq++);

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
