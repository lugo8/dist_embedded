#include "rx.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/can.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "control.h"
#include "io_status.h"
#include "protocol.h"
#include "state.h"

#define RX_QUEUE_DEPTH 256 /* a Pi burst is ~40 ZONE_CTRL frames per bus */
#define BUTTON_B_DOUBLE_PRESS_MS 500

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
	uint8_t bus;
	bool valid;
};

static struct seen seen_tbl[8];
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
		printk("CAN %c degraded: state %d\n", 'A' + bus, state);
	}
	if (io_status_set_can_degraded(bus, degraded)) {
		printk("Failed to update CAN %c degraded status\n", 'A' + bus);
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

static bool is_duplicate(uint16_t id, uint8_t seq, uint8_t bus)
{
	struct seen *free_slot = NULL;

	for (int i = 0; i < ARRAY_SIZE(seen_tbl); i++) {
		struct seen *s = &seen_tbl[i];

		if (s->valid && s->id == id) {
			bool dup = (s->seq == seq) && (s->bus != bus);

			if (!dup) {
				s->seq = seq;
				s->bus = bus;
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
			.bus = bus,
			.valid = true,
		};
	}
	return false;
}

static void handle_zone_control(const uint8_t payload[CAN_PAYLOAD_SIZE])
{
	struct wheel_state ws;

	if (!wheel_frame_valid(payload) ) {
		printk("Rejecting zone-control frame with reserved flags set\n");
		state_note_bad_cmd();
		return;
	}

	decode_wheel_state(payload, &ws);
	state_set_wheel_state(&ws);
	io_status_note_control_received();

	handle_button_b(ws.self_test_btn);

	int rc = control_apply_zone_control(&ws);

	if (rc) {
		printk("Immediate motor output update failed: %d\n", rc);
		io_status_mark_error();
	}
}

static void handle_frame(const struct rx_item *item)
{
	const struct can_frame *frame = &item->frame;
	struct heartbeat hb;

	if (!is_known_msg_type(frame->id)) {
		return;
	}

	if (frame->dlc != CAN_PAYLOAD_SIZE ||
	    (frame->flags & (CAN_FRAME_IDE | CAN_FRAME_RTR))) {
		printk("Rejecting malformed CAN frame 0x%03x on bus %c\n",
		       frame->id, 'A' + item->bus);
		io_status_mark_error();
		if (frame->id == MSG_ZONE_CTRL) {
			state_note_bad_cmd();
		}
		return;
	}

	uint8_t seq = message_sequence(frame->id, frame->data);

	/* Heartbeats from the two buses must be tracked independently. */
	if (frame->id != MSG_HB_COCKPIT &&
	    is_duplicate(frame->id, seq, item->bus)) {
		return;
	}

	switch (frame->id) {
	case MSG_ZONE_CTRL:
		//printk("zone control frame received on bus %c\n", 'A' + item->bus);
		handle_zone_control(frame->data);
		break;
	case MSG_HB_COCKPIT:
		//printk("cockpit heartbeat frame received on bus %c\n", 'A' + item->bus);
		decode_heartbeat(frame->data, &hb);
		state_set_hb_rpi(&hb, item->bus);
		break;
	case MSG_HB_STEERING:
		//printk("steering heartbeat frame received on bus %c\n", 'A' + item->bus);
		decode_heartbeat(frame->data, &hb);
		state_set_hb_steering(&hb);
		break;
	case MSG_HB_DRIVETRAIN:
		//printk("drivetrain heartbeat frame received on bus %c\n", 'A' + item->bus);
		decode_heartbeat(frame->data, &hb);
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
			int rc = blinker_set_message(message.state, message.leds_on);

			if (rc) {
				printk("Failed to update rear blinkers: %d\n", rc);
				io_status_mark_error();
			}
		} else {
			printk("Rejecting malformed blinker payload\n");
			io_status_mark_error();
		}
		break;
	}
	default:
		break;
	}
}

static void rx_thread(void *p1, void *p2, void *p3)
{
	struct rx_item it;

	while (1) {
		k_msgq_get(&rx_q, &it, K_FOREVER);
		//printk("rx_thread: received frame 0x%03x on bus %c\n", it.frame.id, 'A' + it.bus);
		handle_frame(&it);
	}
}

#define RX_STACK_SIZE 1024
#define RX_PRIORITY 3 /* decide this from your Part 3 analysis */

K_THREAD_DEFINE(rx_tid, RX_STACK_SIZE, rx_thread, NULL, NULL, NULL,
		RX_PRIORITY, 0, 0);

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