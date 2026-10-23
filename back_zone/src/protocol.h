#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>

#define MSG_ZONE_CTRL      0x01
#define MSG_HB_COCKPIT     0x02
#define MSG_HB_STEERING    0x03
#define MSG_HB_DRIVETRAIN  0x04
#define MSG_FORCE_FEEDBACK 0x05
#define MSG_BLINKERS       0x06

#define CAN_MAX_PAYLOAD_SIZE 8
#define MSG_ZONE_CTRL_DLC 8
#define MSG_HEARTBEAT_DLC 2
#define MSG_FORCE_FEEDBACK_DLC 3
#define MSG_BLINKERS_DLC 3

#define CTRL_BTN_LEFT       7
#define CTRL_BTN_Y          6
#define CTRL_BTN_A          4
#define CTRL_BTN_B          5
#define CTRL_BTN_X          3
#define CTRL_BTN_RIGHT      0
#define CTRL_RESERVED_MASK  0x06

#define HB_CAN_A_DEGRADED   7
#define HB_CAN_B_DEGRADED   0

enum blinker_state {
	BLINKERS_OFF = 0,
	BLINKERS_LEFT = 1,
	BLINKERS_RIGHT = 2,
	BLINKERS_HAZARD = 3,
};

enum zone_state {
	ZONE_STATE_NORMAL = 0,
	ZONE_STATE_FAILSAFE_LINK_LOST = 1,
	ZONE_STATE_FAILSAFE_SELF_TEST = 2,
	ZONE_STATE_FAILSAFE_BAD_CMD = 3,
	ZONE_STATE_FAILSAFE_RPI_ERROR = 4,
};

struct wheel_state {
	uint8_t seq;
	int16_t steering;
	int16_t throttle;
	int16_t brake;
	bool left_btn;
	bool right_btn;
	bool self_test_btn;
	bool btn_a;
	bool btn_y;
};

struct heartbeat {
	uint8_t seq;
	bool can_a_degraded;
	bool can_b_degraded;
};

struct force_feedback {
	uint8_t seq;
	int16_t force;
};

struct blinkers_message {
	uint8_t state;
	bool leds_on;
	uint8_t seq;
};

bool is_known_msg_type(uint16_t id);
uint8_t message_dlc(uint16_t id);
const char *zone_state_name(uint8_t zone_state);

bool wheel_frame_valid(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE]);
void decode_wheel_state(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE], struct wheel_state *out);
void decode_heartbeat(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE], struct heartbeat *out);
void encode_heartbeat(uint8_t seq, bool can_a_degraded, bool can_b_degraded,
		      uint8_t payload[CAN_MAX_PAYLOAD_SIZE]);
void decode_force_feedback(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE],
			   struct force_feedback *out);
bool decode_blinkers(const uint8_t payload[CAN_MAX_PAYLOAD_SIZE],
		     struct blinkers_message *out);
uint8_t message_sequence(uint16_t id, const uint8_t payload[CAN_MAX_PAYLOAD_SIZE]);

#endif /* PROTOCOL_H_ */
