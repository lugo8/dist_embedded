#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CAN_ID_ZONE_CTRL      0x01 // Cockpit -> Steering, Drivetrain
#define CTRL_BTN_LEFT   7
#define CTRL_BTN_Y      6
#define CTRL_BTN_B      5
#define CTRL_BTN_A      4
#define CTRL_BTN_X      3
#define CTRL_BTN_RIGHT  0
#define CTRL_BTN_RESERVED_MASK 0x06 // bits 2 n 1

#define CAN_ID_HB_COCKPIT     0x02 // Cockpit -> Steering, Drivetrain
#define CAN_ID_HB_STEERING    0x03 // Steering -> Cockpit, Drivetrain
#define CAN_ID_HB_DRIVETRAIN  0x04 // Drivetrain -> Cockpit, Steering
#define HB_BIT_CAN_A_DEGRADED 7
#define HB_BIT_CAN_B_DEGRADED 0
#define HB_RESERVED_MASK      0x7E

#define CAN_ID_FORCE_FEEDBACK 0x05 // Steering -> Cockpit

#define CAN_ID_BLINKER        0x06 // Cockpit -> Steering, Drivetrain

// DLCs include only defined payload bytes.
#define CAN_DLC_ZONE_CTRL      8
#define CAN_DLC_HB             2
#define CAN_DLC_FORCE_FEEDBACK 3
#define CAN_DLC_BLINKER        3

struct zone_ctrl {
	uint8_t buttons;
	int16_t brake;
	int16_t throttle;
	int16_t steering;
	uint8_t seq;
};

struct heartbeat {
	bool can_a_degraded;
	bool can_b_degraded;
	uint8_t seq;
};

struct force_feedback {
	int16_t force;
	uint8_t seq;
};

enum { BLINK_OFF = 0, BLINK_LEFT = 1, BLINK_RIGHT = 2, BLINK_HAZARD = 3 };

struct blinker_msg {
	uint8_t state;
	uint8_t phase;
	uint8_t seq;
};

/* ZONE_CTRL: B0 buttons | B1,B2 brake | B3,B4 throttle | B5,B6 steering | B7 seq.
 * All 16-bit fields go out MSB first (big-endian). */
static inline void ctrl_encode(const struct zone_ctrl *c, uint8_t d[CAN_DLC_ZONE_CTRL])
{
	d[0] = c->buttons;
	d[1] = (uint8_t)((uint16_t)c->brake >> 8);
	d[2] = (uint8_t)((uint16_t)c->brake & 0xFF);
	d[3] = (uint8_t)((uint16_t)c->throttle >> 8);
	d[4] = (uint8_t)((uint16_t)c->throttle & 0xFF);
	d[5] = (uint8_t)((uint16_t)c->steering >> 8);
	d[6] = (uint8_t)((uint16_t)c->steering & 0xFF);
	d[7] = c->seq;
}

static inline void hb_encode(const struct heartbeat *h, uint8_t d[CAN_DLC_HB])
{
	for (int i = 0; i < CAN_DLC_HB; i++) {
		d[i] = 0;
	}
	d[0] = (uint8_t)((h->can_a_degraded << HB_BIT_CAN_A_DEGRADED) |
			 (h->can_b_degraded << HB_BIT_CAN_B_DEGRADED));
	d[1] = h->seq;
}

static inline bool hb_decode(const uint8_t *d, uint8_t dlc, struct heartbeat *out)
{
	if (dlc != CAN_DLC_HB || (d[0] & HB_RESERVED_MASK)) {
		return false;
	}
	out->can_a_degraded = (d[0] >> HB_BIT_CAN_A_DEGRADED) & 1;
	out->can_b_degraded = (d[0] >> HB_BIT_CAN_B_DEGRADED) & 1;
	out->seq = d[1];
	return true;
}

static inline bool force_decode(const uint8_t *d, uint8_t dlc, struct force_feedback *out)
{
	if (dlc != CAN_DLC_FORCE_FEEDBACK) {
		return false;
	}
	out->force = (int16_t)(((uint16_t)d[1] << 8) | d[0]);
	out->seq = d[2];
	return true;
}

static inline void blinker_encode(const struct blinker_msg *b, uint8_t d[CAN_DLC_BLINKER])
{
	for (int i = 0; i < CAN_DLC_BLINKER; i++) {
		d[i] = 0;
	}
	d[0] = b->state;
	d[1] = b->phase;
	d[2] = b->seq;
}
