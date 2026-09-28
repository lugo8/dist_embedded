#include "protocol.h"

uint8_t checksum(const uint8_t *data, size_t len)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < len; i++) {
		sum ^= data[i];
	}

	return sum;
}

/* --------------------------------- RX --------------------------------- */
// check if valid msg
bool is_known_msg_type(uint8_t msg_type)
{
	switch (msg_type) {
		case MSG_WHEEL_STATE:
			return true;
		case MSG_HB_RPI:
			return true;
		case MSG_HB_DRIVETRAIN:
			return true;
		case MSG_FORCE_FDBK:
			return true;
		case MSG_STATUS: // TEMP: only relevant on the self-loopback test rig -
			return true; // the real Pi never echoes this back to the STM32
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
	default:
		return "UNKNOWN";
	}
}

// get wheel info
void decode_wheel_state(const uint8_t *frame, struct wheel_state *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->steering = (data[0] << 8 | data[1]);
	out->throttle = (data[2] << 8 | data[3]);
	out->brake = (data[4] << 8 | data[5]);
	out->left_btn = data[6] >> 7;
	out->right_btn = data[6] & 0x01;
}

// get heartbeat info
void decode_heartbeat(const uint8_t *frame, struct heartbeat *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->line_alive = data[0] >> 7;
}

// get force feedback info
void decode_force_feedback(const uint8_t *frame, struct force_feedback *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->force = (data[0] << 8 | data[1]);
}

void decode_status(const uint8_t *frame, struct status_frame *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->motor_a_current = (data[0] << 8 | data[1]);
	out->motor_b_current = (data[2] << 8 | data[3]);
	out->servo_current = (data[4] << 8 | data[5]);
	out->zone_state = data[6];
}

/* --------------------------------- TX --------------------------------- */
// form payload
void encode_status_data(uint16_t motor_a_current, uint16_t motor_b_current,
			 uint16_t servo_current, uint8_t zone_state, uint8_t data[8])
{
	data[0] = motor_a_current >> 8;
	data[1] = motor_a_current & 0xFF;
	data[2] = motor_b_current >> 8;
	data[3] = motor_b_current & 0xFF;
	data[4] = servo_current >> 8;
	data[5] = servo_current & 0xFF;
	data[6] = zone_state;
	data[7] = 0;
}

// form frame
void encode_frame(uint8_t msg_type, uint8_t seq, const uint8_t data[8], uint8_t *frame_out)
{
	frame_out[0] = msg_type;
	frame_out[1] = seq;

	for (int i = 0; i < 8; i++) {
		frame_out[2 + i] = data[i];
	}

	frame_out[CMD_FRAME_LEN - 1] = checksum(frame_out, CMD_CHK_LEN);
}
