#include "protocol.h"

#include <stddef.h>

static int16_t decode_i16_be(uint8_t msb, uint8_t lsb)
{
	uint16_t value = ((uint16_t)msb << 8) | lsb;

	return value <= INT16_MAX ? (int16_t)value : (int16_t)((int32_t)value - 65536);
}

bool is_known_msg_type(uint16_t id)
{
	switch (id) {
	case MSG_ZONE_CTRL:
	case MSG_HB_COCKPIT:
	case MSG_HB_STEERING:
	case MSG_HB_DRIVETRAIN:
	case MSG_FORCE_FEEDBACK:
	case MSG_BLINKERS:
		return true;
	default:
		return false;
	}
}

const char *zone_state_name(uint8_t zone_state)
{
	switch (zone_state) {
	case ZONE_STATE_NORMAL:
		return "NORMAL";
	case ZONE_STATE_FAILSAFE_LINK_LOST:
		return "FAILSAFE_LINK_LOST";
	case ZONE_STATE_FAILSAFE_SELF_TEST:
		return "FAILSAFE_SELF_TEST";
	case ZONE_STATE_FAILSAFE_BAD_CMD:
		return "FAILSAFE_BAD_CMD";
	case ZONE_STATE_FAILSAFE_RPI_ERROR:
		return "FAILSAFE_RPI_ERROR";
	default:
		return "UNKNOWN";
	}
}

bool wheel_frame_valid(const uint8_t payload[CAN_PAYLOAD_SIZE])
{
	return (payload[0] & CTRL_RESERVED_MASK) == 0;
}

void decode_wheel_state(const uint8_t payload[CAN_PAYLOAD_SIZE], struct wheel_state *out)
{
	out->left_btn = (payload[0] >> CTRL_BTN_LEFT) & 1;
	out->btn_y = (payload[0] >> CTRL_BTN_Y) & 1;
	out->btn_a = (payload[0] >> CTRL_BTN_A) & 1;
	out->self_test_btn = (payload[0] >> CTRL_BTN_B) & 1;
	out->right_btn = (payload[0] >> CTRL_BTN_RIGHT) & 1;
	out->brake = decode_i16_be(payload[1], payload[2]);
	out->throttle = decode_i16_be(payload[3], payload[4]);
	out->steering = decode_i16_be(payload[5], payload[6]);
	out->seq = payload[7];
}

void decode_heartbeat(const uint8_t payload[CAN_PAYLOAD_SIZE], struct heartbeat *out)
{
	out->can_a_degraded = (payload[0] >> HB_CAN_A_DEGRADED) & 1;
	out->can_b_degraded = (payload[0] >> HB_CAN_B_DEGRADED) & 1;
	out->seq = payload[1];
}

void encode_heartbeat(uint8_t seq, bool can_a_degraded, bool can_b_degraded,
		      uint8_t payload[CAN_PAYLOAD_SIZE])
{
	for (size_t i = 0; i < CAN_PAYLOAD_SIZE; i++) {
		payload[i] = 0;
	}

	payload[0] = (can_a_degraded << HB_CAN_A_DEGRADED) |
		     (can_b_degraded << HB_CAN_B_DEGRADED);
	payload[1] = seq;
}

void decode_force_feedback(const uint8_t payload[CAN_PAYLOAD_SIZE],
			   struct force_feedback *out)
{
	out->force = decode_i16_be(payload[1], payload[0]);
	out->seq = payload[2];
}

bool decode_blinkers(const uint8_t payload[CAN_PAYLOAD_SIZE],
		     struct blinkers_message *out)
{
	if (payload[0] > BLINKERS_HAZARD || payload[1] > 1) {
		return false;
	}

	out->state = payload[0];
	out->leds_on = payload[1] != 0;
	out->seq = payload[2];
	return true;
}

uint8_t message_sequence(uint16_t id, const uint8_t payload[CAN_PAYLOAD_SIZE])
{
	switch (id) {
	case MSG_ZONE_CTRL:
		return payload[7];
	case MSG_HB_COCKPIT:
	case MSG_HB_STEERING:
	case MSG_HB_DRIVETRAIN:
		return payload[1];
	case MSG_FORCE_FEEDBACK:
	case MSG_BLINKERS:
		return payload[2];
	default:
		return 0;
	}


}
