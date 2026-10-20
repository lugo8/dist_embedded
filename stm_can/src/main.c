#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#define ID_HB_N2 0x101   /* change per node */

static const struct device *const can_dev[2] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};

CAN_MSGQ_DEFINE(rx_q_a, 16);
CAN_MSGQ_DEFINE(rx_q_b, 16);
static struct k_msgq *const rx_q[2] = { &rx_q_a, &rx_q_b };

static uint8_t seq;  /* 4-bit counter, bumped once per message */

static void tx_cb(const struct device *dev, int err, void *arg)
{
	if (err) {
		printk("TX error %d on bus %c\n", err, (int)(intptr_t)arg);
	}
}

/* Queue the same frame on both controllers, bump seq once. */
static void send_both(uint32_t id, uint8_t flags, const uint8_t payload[7])
{
	struct can_frame f = { .id = id, .dlc = 8, .flags = 0 };

	f.data[0] = (flags << 4) | (seq & 0x0F);
	memcpy(&f.data[1], payload, 7);

	for (int i = 0; i < 2; i++) {
		int rc = can_send(can_dev[i], &f, K_MSEC(5), tx_cb,
				  (void *)(intptr_t)('A' + i));
		if (rc) {
			printk("can_send bus %c failed: %d\n", 'A' + i, rc);
		}
	}
	seq = (seq + 1) & 0x0F;
}

static int can_bringup(int i)
{
	const struct device *dev = can_dev[i];
	/* accept everything for now; tighten to catalog IDs later */
	const struct can_filter any = { .id = 0, .mask = 0, .flags = 0 };
	enum can_state st;
	struct can_bus_err_cnt cnt;
	int rc;

	if (!device_is_ready(dev)) {
		printk("CAN %c not ready\n", 'A' + i);
		return -ENODEV;
	}

	/* Uncomment to test SPI wiring with no bus attached at all: */
	/* can_set_mode(dev, CAN_MODE_LOOPBACK); */

	rc = can_start(dev);
	if (rc) {
		printk("can_start %c: %d\n", 'A' + i, rc);
		return rc;
	}

	rc = can_add_rx_filter_msgq(dev, rx_q[i], &any);
	if (rc < 0) {
		printk("add filter %c: %d\n", 'A' + i, rc);
		return rc;
	}

	can_get_state(dev, &st, &cnt);
	printk("CAN %c state=%d tx_err=%u rx_err=%u\n",
	       'A' + i, st, cnt.tx_err_cnt, cnt.rx_err_cnt);
	return 0;
}

int main(void)
{
	for (int i = 0; i < 2; i++) {
		if (can_bringup(i)) {
			return 0;
		}
	}

	uint8_t payload[7] = { 0 };

	while (1) {
		send_both(ID_HB_N2, 0, payload);
		payload[0]++;

		struct can_frame rx;
		for (int i = 0; i < 2; i++) {
			while (k_msgq_get(rx_q[i], &rx, K_NO_WAIT) == 0) {
				printk("RX bus %c id=0x%03x seq=%u b1=%u\n",
				       'A' + i, rx.id, rx.data[0] & 0x0F,
				       rx.data[1]);
			}
		}
		k_sleep(K_MSEC(100));
	}
}