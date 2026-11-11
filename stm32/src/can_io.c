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
static const struct gpio_dt_spec ref_rx_pin = GPIO_DT_SPEC_GET(USER, ref_rx_gpios);

/*
 * Zephyr's MCP2515 driver uses one TX buffer (MCP2515_TX_CNT = 1), so a frame queued
 * while another is still going out has to wait for it. 3 ms covers a few 270 us frames
 * ahead of it; on a dead bus the send gives up after that instead of blocking forever.
 */
#define TX_WAIT K_MSEC(3)

struct rx_item {
	struct can_frame f;
	int64_t rx_us; /* when the frame finished arriving: the time sync pairs these */
	uint8_t bus;   /* where it came from, or for the relay queue, where it goes */
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), 16, 4);

/* runs in the MCP2515 driver thread: stamp, copy, hand off, nothing else */
static void rx_cb(const struct device *dev, struct can_frame *f, void *user)
{
	struct rx_item it = { .rx_us = local_us(), .f = *f, .bus = (uint8_t)(uintptr_t)user };

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

/* REF: each bus gets its own payload (its own follow-up time) under one seq */
static void (*ref_done_cb)(int bus);

static void ref_tx_done(const struct device *dev, int err, void *user)
{
	ARG_UNUSED(dev);
	if (!err && ref_done_cb) {
		ref_done_cb((int)(uintptr_t)user);
	}
}

void send_ref(uint16_t id, atomic_t *seq, uint8_t d[2][MSG_DLC], void (*done)(int bus))
{
	uint8_t s = atomic_inc(seq) & 0x0F;

	ref_done_cb = done;
	for (int bus = 0; bus < 2; bus++) {
		struct can_frame f = { .id = id, .dlc = MSG_DLC };

		d[bus][0] = (d[bus][0] & 0xF0) | s;
		memcpy(f.data, d[bus], MSG_DLC);
		gpio_pin_set_dt(&tx_pin[bus], 1);
		can_send(can_dev[bus], &f, TX_WAIT, ref_tx_done, (void *)(uintptr_t)bus);
		gpio_pin_set_dt(&tx_pin[bus], 0);
	}
}

/* RELAY_TX is a test point on N2 only; N3 uses that pin for PWM_SET */
static const struct gpio_dt_spec relay_pin = GPIO_DT_SPEC_GET_OR(USER, relay_tx_gpios, {0});

K_MSGQ_DEFINE(relay_q, sizeof(struct rx_item), 16, 4);

atomic_t relay_zone; /* written by the raft agent from route_relay() */

/*
 * Forward-on-detect, as in Lab 3: once a zone is missing on one bus while it is still
 * talking on the other, everything it sends is copied across. Who does it comes from
 * route_relay() (route.h): N2, else N3 when N2 is off a bus, else the Pi, and never a node
 * the Raft network graph has off a bus. The same choice on every node.
 * A forwarded copy carries FLAG_RELAYED and is never forwarded again, and a receiver
 * that also got the direct copy drops the relayed one as a duplicate.
 */
static void relay_offer(const struct rx_item *it, int zone)
{
	int to = 1 - it->bus;

	if (atomic_get(&relay_zone) != MY_ZONE) {
		return;
	}
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
	    zone < ZONE_N1 || zone > ZONE_N3 || zone == MY_ZONE) {
		return;
	}
	bool relayed = f->data[0] & FLAG_RELAYED;

	/* both copies count for liveness, only the first one goes on to the tasks */
	monitor_rx(it->bus, zone, is_hb(f->id), relayed);
	bool copy = dedup_is_copy(&dd[idx], SEQ(f->data), it->bus);

	if (!copy && !relayed) {
		relay_offer(it, zone);
	}
	/* brake path first: nothing below may delay CTRL */
	if (!copy && f->id == ID_CTRL) {
		struct ctrl c;
		bool ok = ctrl_decode(f->data, &c);

		monitor_ctrl(ok);
		if (ok) {
			actuator_ctrl(&c);
			blinker_ctrl(c.blink, c.turn_slot);
		}
	}
	if (!copy && raft_base(f->id) == ID_REF_BASE) {
		gpio_pin_toggle_dt(&ref_rx_pin);
	}
	/* Raft, routing and time sync: every copy, through the agent's queue */
	if (is_raft(f->id) || is_hb(f->id)) {
		struct raft_rx r = { .bus = it->bus, .relayed = relayed, .first = !copy,
				     .rx_us = it->rx_us };

		r.f.id = (uint16_t)f->id;
		memcpy(r.f.data, f->data, MSG_DLC);
		raft_agent_rx(&r);
	}
	/* FAULT and FORCE need nothing here; BLINK (Lab 3) is no longer sent */
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
#define FILTER(i, m) { .id = (i), .mask = (m) },
	static const struct can_filter filters[] = { CAN_RX_FILTERS(FILTER) };
#undef FILTER

	for (int bus = 0; bus < 2; bus++) {
		if (!device_is_ready(can_dev[bus]) ||
		    gpio_pin_configure_dt(&rx_pin[bus], GPIO_OUTPUT_INACTIVE) ||
		    gpio_pin_configure_dt(&tx_pin[bus], GPIO_OUTPUT_INACTIVE)) {
			return -ENODEV;
		}
	}
	if (gpio_pin_configure_dt(&ref_rx_pin, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	if (relay_pin.port && gpio_pin_configure_dt(&relay_pin, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	atomic_set(&relay_zone, ZONE_NONE);
	k_thread_start(relay_tid);
	k_thread_start(dispatch_tid);

	for (int bus = 0; bus < 2; bus++) {
		for (size_t i = 0; i < ARRAY_SIZE(filters); i++) {
			if (can_add_rx_filter(can_dev[bus], rx_cb, (void *)(uintptr_t)bus,
					      &filters[i]) < 0) {
				return -EIO;
			}
		}
		if (can_start(can_dev[bus])) {
			return -EIO;
		}
	}
	return 0;
}
