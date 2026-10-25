#include "rx.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "control.h"
#include "edf.h"
#include "io_status.h"
#include "protocol.h"
#include "state.h"

#define RX_QUEUE_DEPTH 256 /* a Pi burst is ~40 ZONE_CTRL frames per bus */
#define BUTTON_B_DOUBLE_PRESS_MS 500
#define RELAY_DUPLICATE_WINDOW_MS LINK_TIMEOUT_MS
#define CAN_RELAY_ENABLED 0

extern const k_tid_t rx_tid;

static const struct device *const can_dev[2] = {
	DEVICE_DT_GET(DT_NODELABEL(can_a)),
	DEVICE_DT_GET(DT_NODELABEL(can_b)),
};

struct rx_item {
	uint8_t bus;              /* 0 = A, 1 = B */
	struct can_frame frame;
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), RX_QUEUE_DEPTH, 4);
static atomic_t rx_overflow_reported;

/* Runs in the MCP2515 driver's work thread (not an ISR). Keep it short:
 * copy the frame and hand it to our own thread. */
static void can_rx_cb(const struct device *dev, struct can_frame *frame,
		      void *user_data)
{
	ARG_UNUSED(dev);

	struct rx_item it = { .bus = (uint8_t)(uintptr_t)user_data, .frame = *frame };

	if (io_status_can_rx(it.bus)) {
		io_status_mark_error();
	}
	edf_deadline_after_ms(rx_tid, 5, EDF_TIE_CAN_RX);
	if (k_msgq_put(&rx_q, &it, K_NO_WAIT)) {
		if (atomic_cas(&rx_overflow_reported, 0, 1)) {
			printk("CAN RX software queue full; dropping frames\n");
		}
		/* A dropped frame is not a zone fault: the next ZONE_CTRL replaces it. */
	}
}

struct seen {
	uint16_t id;
	uint8_t seq;
	int64_t seen_ms;
	bool valid;
};

static struct seen seen_tbl[8];
enum message_source {
	SOURCE_COCKPIT,
	SOURCE_STEERING,
	SOURCE_DRIVETRAIN,
	SOURCE_COUNT,
};
static int64_t source_heartbeat_ms[SOURCE_COUNT][2] = {
	{ -(LINK_TIMEOUT_MS + 1), -(LINK_TIMEOUT_MS + 1) },
	{ -(LINK_TIMEOUT_MS + 1), -(LINK_TIMEOUT_MS + 1) },
	{ -(LINK_TIMEOUT_MS + 1), -(LINK_TIMEOUT_MS + 1) },
};
static bool previous_button_b;
static bool button_b_press_pending;
static int64_t button_b_first_press_ms;

static void can_state_changed(const struct device *dev, enum can_state state,
			      struct can_bus_err_cnt err_cnt, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(err_cnt);

	uint8_t bus = (uint8_t)(uintptr_t)user_data;
	bool degraded = state != CAN_STATE_ERROR_ACTIVE;

	if (degraded) {
		// printk("CAN %c degraded: state %d\n", 'A' + bus, state);
	}
	if (io_status_set_can_degraded(bus, degraded)) {
		// printk("Failed to update CAN %c degraded status\n", 'A' + bus);
	}
}

static void handle_button_b(bool pressed)
{
	int64_t now = k_uptime_get();

	if (pressed && !previous_button_b) {
		if (button_b_press_pending &&
		    now - button_b_first_press_ms <= BUTTON_B_DOUBLE_PRESS_MS) {
			button_b_press_pending = false;
			io_status_clear_error();
		} else {
			button_b_press_pending = true;
			button_b_first_press_ms = now;
			io_status_enter_error(ZONE_STATE_FAILSAFE_SELF_TEST);
		}
	} else if (button_b_press_pending &&
		   now - button_b_first_press_ms > BUTTON_B_DOUBLE_PRESS_MS) {
		button_b_press_pending = false;
	}

	previous_button_b = pressed;
}

static int message_source(uint16_t id)
{
	switch (id) {
	case MSG_ZONE_CTRL:
	case MSG_BLINKERS:
		return SOURCE_COCKPIT;
	case MSG_HB_STEERING:
		return SOURCE_STEERING;
	case MSG_HB_DRIVETRAIN:
	case MSG_FORCE_FEEDBACK:
		return SOURCE_DRIVETRAIN;
	default:
		return -1;
	}
}

static void note_source_heartbeat(uint16_t id, uint8_t bus)
{
	int source;

	switch (id) {
	case MSG_ZONE_CTRL:
		source = SOURCE_COCKPIT;
		break;
	case MSG_HB_STEERING:
		source = SOURCE_STEERING;
		break;
	case MSG_HB_DRIVETRAIN:
		source = SOURCE_DRIVETRAIN;
		break;
	default:
		return;
	}

	source_heartbeat_ms[source][bus] = k_uptime_get();
}

static bool is_duplicate(uint16_t id, uint8_t seq)
{
	struct seen *free_slot = NULL;
	int64_t now = k_uptime_get();

	for (int i = 0; i < ARRAY_SIZE(seen_tbl); i++) {
		struct seen *s = &seen_tbl[i];

		if (s->valid && s->id == id) {
			bool dup = s->seq == seq &&
				   now - s->seen_ms < RELAY_DUPLICATE_WINDOW_MS;

			if (!dup) {
				s->seq = seq;
				s->seen_ms = now;
			}
			return dup;
		}
		if (!free_slot && !s->valid) {
			free_slot = s;
		}
	}
	if (free_slot) {
		*free_slot = (struct seen){
			.id = id,
			.seq = seq,
			.seen_ms = now,
			.valid = true,
		};
	}
	return false;
}

static void relay_if_source_bus_only(const struct rx_item *item, int source)
{
	if (!CAN_RELAY_ENABLED || source < 0) {
		return;
	}

	uint8_t from_bus = item->bus;
	uint8_t to_bus = 1U - from_bus;
	int64_t now = k_uptime_get();
	bool source_on_ingress =
		now - source_heartbeat_ms[source][from_bus] < LINK_TIMEOUT_MS;
	bool source_on_egress =
		now - source_heartbeat_ms[source][to_bus] < LINK_TIMEOUT_MS;

	if (!source_on_ingress || source_on_egress ||
	    io_status_is_can_degraded(to_bus)) {
		return;
	}

	int rc = can_send(can_dev[to_bus], &item->frame, K_MSEC(5), NULL, NULL);

	if (rc) {
		// printk("CAN relay %c->%c failed for 0x%03x: %d\n",
		       // 'A' + from_bus, 'A' + to_bus, item->frame.id, rc);
		(void)io_status_set_can_degraded(to_bus, true);
	}
}

static void handle_zone_control(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE])
{
	struct wheel_state ws;

	decode_wheel_state(payload, &ws);
	control_schedule_frame();
	state_set_wheel_state(&ws);

	handle_button_b(ws.self_test_btn);
}

static void handle_frame(const struct rx_item *item)
{
	const struct can_frame *frame = &item->frame;
	struct heartbeat hb;

	if (!is_known_msg_type(frame->id)) {
		return;
	}

	if (frame->dlc != message_dlc(frame->id) ||
	    (frame->flags & (CAN_FRAME_IDE | CAN_FRAME_RTR))) {
		// printk("Rejecting malformed CAN frame 0x%03x on bus %c\n",
		       // frame->id, 'A' + item->bus);
		io_status_mark_error();
		if (frame->id == MSG_ZONE_CTRL) {
			state_note_bad_cmd();
		}
		return;
	}

	if (frame->id == MSG_ZONE_CTRL) {
		if (!wheel_frame_valid(frame->data)) {
			// printk("Rejecting zone-control frame with reserved flags set\n");
			state_note_bad_cmd();
			return;
		}
		state_note_zone_control(item->bus);
	}

	uint8_t seq = message_sequence(frame->id, frame->data);
	int source = message_source(frame->id);

	if (is_duplicate(frame->id, seq)) {
		if (frame->id == MSG_ZONE_CTRL) {
			io_status_note_control_received();
		}
		return;
	}
	/* A relayed copy has the same ID/sequence and must not make its source
	 * appear directly reachable on the opposite bus. */
	note_source_heartbeat(frame->id, item->bus);
	relay_if_source_bus_only(item, source);

	switch (frame->id) {
	case MSG_ZONE_CTRL:
		//printk("zone control frame received on bus %c\n", 'A' + item->bus);
		handle_zone_control(frame->data);
		break;
	case MSG_HB_STEERING:
		//printk("steering heartbeat frame received on bus %c\n", 'A' + item->bus);
		decode_heartbeat(frame->data, &hb);
		state_set_hb_steering(&hb);
		blinker_hazard_sync(hb.seq);
		break;
	case MSG_HB_DRIVETRAIN:
		//printk("drivetrain heartbeat frame received on bus %c\n", 'A' + item->bus);
		decode_drivetrain_heartbeat(frame->data, &hb);
		state_set_hb_drivetrain(&hb);
		break;
	case MSG_FORCE_FEEDBACK: {
		//printk("force feedback frame received on bus %c\n", 'A' + item->bus);
		struct force_feedback feedback;

		decode_force_feedback(frame->data, &feedback);
		state_set_force_feedback(&feedback);
		break;
	}
	case MSG_BLINKERS: {
		//printk("blinkers frame received on bus %c\n", 'A' + item->bus);
		struct blinkers_message message;

		if (decode_blinkers(frame->data, &message)) {
			/* TEMP debug: when each BLINKER edge arrives, to compare against the other zone */
			//printk("%s BLK rx t=%u bus=%c state=%u on=%d\n", "BACK", k_uptime_get_32(),
			       //'A' + item->bus, message.state, message.leds_on);
			int rc = blinker_set_message(message.state, message.leds_on);

			if (rc) {
				// printk("Failed to update rear blinkers: %d\n", rc);
				io_status_mark_error();
			}
		} else {
			// printk("Rejecting malformed blinker payload\n");
			io_status_mark_error();
		}
		break;
	}
	default:
		break;
	}

	if (frame->id == MSG_ZONE_CTRL) {
		io_status_note_control_received();
	}
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	struct rx_item it;

	while (1) {
		k_msgq_get(&rx_q, &it, K_FOREVER);
		printk("rx_thread: received frame 0x%03x on bus %c\n", it.frame.id, 'A' + it.bus);
		handle_frame(&it);
	}
}

#define RX_STACK_SIZE 1024
K_THREAD_DEFINE(rx_tid, RX_STACK_SIZE, rx_thread, NULL, NULL, NULL,
		EDF_THREAD_PRIORITY, 0, 0);

int rx_init(void)
{
	/* All supported standard IDs (0x01-0x06) are in the 0x00-0x07 range. */
	const struct can_filter known_message_range = {
		.id = 0,
		.mask = 0x7F8,
		.flags = 0,
	};

	for (int i = 0; i < 2; i++) {
		if (!device_is_ready(can_dev[i])) {
			return -ENODEV;
		}
		int fid = can_add_rx_filter(can_dev[i], can_rx_cb,
					    (void *)(uintptr_t)i, &known_message_range);
		if (fid < 0) {
			io_status_mark_error();
			return fid;
		}
		can_set_state_change_callback(can_dev[i], can_state_changed,
					      (void *)(uintptr_t)i);
		int rc = can_start(can_dev[i]);

		if (rc && rc != -EALREADY) {
			io_status_mark_error();
			return rc;
		}
		enum can_state state;

		rc = can_get_state(can_dev[i], &state, NULL);
		if (rc) {
			io_status_mark_error();
			return rc;
		}
		if (io_status_set_can_degraded(i, state != CAN_STATE_ERROR_ACTIVE)) {
			return -EIO;
		}
	}
	return 0;
}