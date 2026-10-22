#ifndef PROTOCOL_H_
#define PROTOCOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// frame: [msg type(1B)][seq(1B)][data(8B)][checksum(1B)]
#define MSG_WHEEL_STATE 0x01 // steering(2B) throttle(2B) brake(2B) btn(1B)
#define MSG_HB_RPI      0x02 // line_alive(1B): bit7=alive
#define MSG_HB_DRIVETRAIN 0x03 // line_alive(1B): bit7=alive
#define MSG_STATUS      0x04 // motor_a_current(2B) motor_b_current(2B) servo_current(2B) zone_state(1B) reserved(1B)
#define MSG_FORCE_FDBK  0x05 // force(2B)

#define CMD_CHK_LEN   (10) // msg_type + seq + data
#define CMD_FRAME_LEN (CMD_CHK_LEN + 1)      // + checksum

// button byte: bit7 = left blinker, bit0 = right blinker, bits 4/3/2 = A/B/X (set by the Pi receiver)
#define BTN_BIT_LEFT       7
#define BTN_BIT_SELF_TEST  3 // B button
#define BTN_BIT_RIGHT      0
#define BTN_RESERVED_MASK  0x62 // bits 6, 5, 1 are never 1

struct wheel_state {
	uint8_t seq;
	int16_t steering;
	int16_t throttle;
	int16_t brake;
	bool left_btn;
	bool right_btn;
	bool self_test_btn;
	bool rpi_error;
};

struct heartbeat {
	uint8_t seq;
	bool line_alive;
};

struct force_feedback {
	uint8_t seq;
	int16_t force;
};

// TEMP: loopback test only - real Pi never sends this back to the STM32
struct status_frame {
	uint8_t seq;
	uint16_t motor_a_current;
	uint16_t motor_b_current;
	uint16_t servo_current;
	uint8_t zone_state;
};

enum zone_state {
	ZONE_STATE_NORMAL = 0,
	ZONE_STATE_FAILSAFE_LINK_LOST = 1,
	ZONE_STATE_FAILSAFE_SELF_TEST = 2, // add when there's button
	ZONE_STATE_FAILSAFE_BAD_CMD = 3,   // for out-of-range detection
	ZONE_STATE_FAILSAFE_RPI_ERROR = 4, // the Pi reported its own error state
};

uint8_t checksum(const uint8_t *data, size_t len);

bool is_known_msg_type(uint8_t msg_type);

const char *zone_state_name(uint8_t zone_state);

bool wheel_frame_valid(const uint8_t *frame);
void decode_wheel_state(const uint8_t *frame, struct wheel_state *out);
void decode_heartbeat(const uint8_t *frame, struct heartbeat *out);
void decode_force_feedback(const uint8_t *frame, struct force_feedback *out);
void decode_status(const uint8_t *frame, struct status_frame *out);

void encode_status_data(uint16_t motor_a_current, uint16_t motor_b_current,
			 uint16_t servo_current, uint8_t zone_state, uint8_t data[8]);
void encode_frame(uint8_t msg_type, uint8_t seq, const uint8_t data[8], uint8_t *frame_out);

#endif /* PROTOCOL_H_ */
