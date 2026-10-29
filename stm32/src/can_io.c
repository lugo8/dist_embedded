/* both CAN controllers: receive + dedup + dispatch (CAN_RX), send on both buses, RELAY */
#include <string.h>
#include <zephyr/drivers/can.h>
#include <zephyr/drivers/gpio.h>
#include "zone.h"

#define USER DT_PATH(zephyr_user)

static const struct device *const can_dev[2] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};
static const struct gpio_dt_spec rx_pin[2] = {
	GPIO_DT_SPEC_GET(USER, can_rx_a_gpios),
	GPIO_DT_SPEC_GET(USER, can_rx_b_gpios),
};
static const struct gpio_dt_spec tx_pin[2] = {
	GPIO_DT_SPEC_GET(USER, can_tx_a_gpios),
	GPIO_DT_SPEC_GET(USER, can_tx_b_gpios),
};

/*
 * Zephyr's MCP2515 driver uses one TX buffer (MCP2515_TX_CNT = 1), so a frame queued
 * while another is still going out has to wait for it. 3 ms covers a few 270 us frames
 * ahead of it; on a dead bus the send gives up after that instead of blocking forever.
 */
#define TX_WAIT K_MSEC(3)

struct rx_item {
	struct can_frame f;
	uint8_t bus; /* where it came from, or for the relay queue, where it goes */
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), 16, 4);

/* runs in the MCP2515 driver thread: copy the frame and hand it off, nothing else */
static void rx_cb(const struct device *dev, struct can_frame *f, void *user)
{
	struct rx_item it = { .f = *f, .bus = (uint8_t)(uintptr_t)user };

	ARG_UNUSED(dev);
	k_msgq_put(&rx_q, &it, K_NO_WAIT);
}

/* a non-NULL callback makes can_send return right away instead of waiting for the bus */
static void tx_done(const struct device *dev, int err, void *user)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(err);
	ARG_UNUSED(user);
}

/* queue one frame on both controllers back to back; seq goes up once per message */
void send_both(uint16_t id, atomic_t *seq, uint8_t d[MSG_DLC])
{
	struct can_frame f = { .id = id, .dlc = MSG_DLC };

	d[0] = (d[0] & 0xF0) | (atomic_inc(seq) & 0x0F);
	memcpy(f.data, d, MSG_DLC);
	for (int bus = 0; bus < 2; bus++) {
		gpio_pin_set_dt(&tx_pin[bus], 1);
		/* a dead bus just loses its copy; the other bus still carries the frame */
		can_send(can_dev[bus], &f, TX_WAIT, tx_done, NULL);
		gpio_pin_set_dt(&tx_pin[bus], 0);
	}
}

/* RELAY_TX is a test point on N2 only; N3 uses that pin for PWM_SET */
static const struct gpio_dt_spec relay_pin = GPIO_DT_SPEC_GET_OR(USER, relay_tx_gpios, {0});

K_MSGQ_DEFINE(relay_q, sizeof(struct rx_item), 16, 4);

/*
 * Forward-on-detect, on both STM32s: once a zone's own heartbeat is missing on one bus
 * while it is still talking on the other, everything it sends is copied across. N2
 * relays in checkoff cases 1-2, N3 in cases 3-4. A forwarded copy carries FLAG_RELAYED
 * and is never forwarded again, so two relays can't bounce a frame between them, and a
 * receiver that also got the direct copy drops the relayed one as a duplicate.
 */
static void relay_offer(const struct rx_item *it, int zone)
{
	int to = 1 - it->bus;

#ifndef ZONE_FRONT
	/*
	 * N2 is the main relay; N3 steps in only when N2 is missing from a bus (cases 3-4).
	 * If both relayed the same frame onto a bus where only they are left (N1 pulled),
	 * nobody would be receiving to ACK it: both retransmit in lockstep forever and
	 * the bus jams.
	 */
	if (monitor_hb_on_bus(ZONE_N2, 0) && monitor_hb_on_bus(ZONE_N2, 1)) {
		return;
	}
#endif
	if (monitor_hb_on_bus(zone, to) || monitor_bus_degraded(to)) {
		return;
	}
	struct rx_item out = { .f = it->f, .bus = to };
	out.f.data[0] |= FLAG_RELAYED;
	k_msgq_put(&relay_q, &out, K_NO_WAIT);
}

static void relay_task(void *p1, void *p2, void *p3)
{
	struct rx_item it;

	while (1) {
		k_msgq_get(&relay_q, &it, K_FOREVER);
		if (relay_pin.port) {
			gpio_pin_set_dt(&relay_pin, 1);
		}
		can_send(can_dev[it.bus], &it.f, TX_WAIT, tx_done, NULL);
		if (relay_pin.port) {
			gpio_pin_set_dt(&relay_pin, 0);
		}
	}
}

K_THREAD_DEFINE(relay_tid, 1024, relay_task, NULL, NULL, NULL, PRIO_RELAY, 0, SYS_FOREVER_MS);

static struct dedup dd[MSG_COUNT];

static void handle(const struct rx_item *it)
{
	const struct can_frame *f = &it->f;
	int idx = msg_index(f->id);
	int zone = msg_sender(f->id, f->data);

	if (f->dlc != MSG_DLC || (f->flags & (CAN_FRAME_IDE | CAN_FRAME_RTR)) || idx < 0 ||
	    zone < ZONE_N1 || zone > ZONE_N3) {
		return;
	}
	bool relayed = f->data[0] & FLAG_RELAYED;

	/* both copies count for liveness, only the first one goes on to the tasks */
	monitor_rx(it->bus, zone, is_hb(f->id), relayed);
	if (dedup_is_copy(&dd[idx], SEQ(f->data), it->bus)) {
		return;
	}
	if (!relayed) {
		relay_offer(it, zone);
	}

	switch (f->id) {
	case ID_CTRL: {
		struct ctrl c;
		bool ok = ctrl_decode(f->data, &c);

		monitor_ctrl(ok);
		if (ok) {
			actuator_ctrl(&c);
		}
		break;
	}
	case ID_BLINK:
		blinker_msg(f->data[1], f->data[2]);
		break;
	default:
		break; /* heartbeats were noted above; FAULT and FORCE need nothing here */
	}
}

/* CAN_RX dispatch: one path for both buses */
static void dispatch_task(void *p1, void *p2, void *p3)
{
	struct rx_item it;

	while (1) {
		k_msgq_get(&rx_q, &it, K_FOREVER);
		gpio_pin_set_dt(&rx_pin[it.bus], 1);
		handle(&it);
		gpio_pin_set_dt(&rx_pin[it.bus], 0);
	}
}

K_THREAD_DEFINE(dispatch_tid, 1024, dispatch_task, NULL, NULL, NULL, PRIO_DISPATCH, 0,
		SYS_FOREVER_MS);

int can_io_init(void)
{
	static const uint16_t ids[] = {
		ID_CTRL, ID_FAULT, ID_HB_N1, ID_HB_N2, ID_HB_N3, ID_BLINK, ID_FORCE,
	};

	for (int bus = 0; bus < 2; bus++) {
		if (!device_is_ready(can_dev[bus]) ||
		    gpio_pin_configure_dt(&rx_pin[bus], GPIO_OUTPUT_INACTIVE) ||
		    gpio_pin_configure_dt(&tx_pin[bus], GPIO_OUTPUT_INACTIVE)) {
			return -ENODEV;
		}
	}
	if (relay_pin.port && gpio_pin_configure_dt(&relay_pin, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	k_thread_start(relay_tid);
	k_thread_start(dispatch_tid);

	for (int bus = 0; bus < 2; bus++) {
		for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
			struct can_filter flt = { .id = ids[i], .mask = CAN_STD_ID_MASK };

			if (can_add_rx_filter(can_dev[bus], rx_cb, (void *)(uintptr_t)bus, &flt) < 0) {
				return -EIO;
			}
		}
		if (can_start(can_dev[bus])) {
			return -EIO;
		}
	}
	return 0;
}
