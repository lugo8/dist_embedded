#pragma once
#include <stdbool.h>
#include <stdint.h>

#define CAN_ID_ZONE_CTRL      0x01 // Cockpit -> Steering, Drivetrain
#define CTRL_BTN_LEFT   7
#define CTRL_BTN_Y      6
#define CTRL_BTN_A      5
#define CTRL_BTN_X      4
#define CTRL_BTN_B      1
#define CTRL_BTN_RIGHT  0
#define CTRL_BTN_RESERVED_MASK 0x0C // bits 3 n 2

#define CAN_ID_HB_COCKPIT     0x02 // Cockpit -> Steering, Drivetrain
#define CAN_ID_HB_STEERING    0x03 // Steering -> Cockpit, Drivetrain
#define CAN_ID_HB_DRIVETRAIN  0x04 // Drivetrain -> Cockpit, Steering
#define HB_BIT_CAN_A_DEGRADED 7
#define HB_BIT_CAN_B_DEGRADED 0
#define HB_RESERVED_MASK      0x7E

#define CAN_ID_FORCE_FEEDBACK 0x05 // Steering -> Cockpit

// every message is DLC 8; bytes after the last defined one are reserved and must be 0
#define CAN_DLC_ZONE_CTRL      8
#define CAN_DLC_HB             8
#define CAN_DLC_FORCE_FEEDBACK 8

#define HB_USED_BYTES    2 // B0 flags, B1 seq
#define FORCE_USED_BYTES 3 // B1,B0 force, B2 seq

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

/* ZONE_CTRL: B0 buttons | B1,B2 brake | B4,B3 throttle | B6,B5 steering | B7 seq.
 * ASSUMPTION: "B1,B2" / "B4,B3" / "B6,B5" are listed most-significant byte first,
 * which makes brake big-endian and throttle/steering little-endian as written.
 * If the STM side reads them differently, change only ctrl_encode. */
static inline void ctrl_encode(const struct zone_ctrl *c, uint8_t d[CAN_DLC_ZONE_CTRL])
{
	d[0] = c->buttons;
	d[1] = (uint8_t)((uint16_t)c->brake >> 8);
	d[2] = (uint8_t)((uint16_t)c->brake & 0xFF);
	d[4] = (uint8_t)((uint16_t)c->throttle >> 8);
	d[3] = (uint8_t)((uint16_t)c->throttle & 0xFF);
	d[6] = (uint8_t)((uint16_t)c->steering >> 8);
	d[5] = (uint8_t)((uint16_t)c->steering & 0xFF);
	d[7] = c->seq;
}

// true if d[from..7] are all 0 (the reserved padding)
static inline bool pad_is_zero(const uint8_t *d, int from)
{
	for (int i = from; i < 8; i++) {
		if (d[i]) {
			return false;
		}
	}
	return true;
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
	if (dlc != CAN_DLC_HB || (d[0] & HB_RESERVED_MASK) || !pad_is_zero(d, HB_USED_BYTES)) {
		return false;
	}
	out->can_a_degraded = (d[0] >> HB_BIT_CAN_A_DEGRADED) & 1;
	out->can_b_degraded = (d[0] >> HB_BIT_CAN_B_DEGRADED) & 1;
	out->seq = d[1];
	return true;
}

static inline bool force_decode(const uint8_t *d, uint8_t dlc, struct force_feedback *out)
{
	if (dlc != CAN_DLC_FORCE_FEEDBACK || !pad_is_zero(d, FORCE_USED_BYTES)) {
		return false;
	}
	out->force = (int16_t)(((uint16_t)d[1] << 8) | d[0]);
	out->seq = d[2];
	return true;
}
