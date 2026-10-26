/*
 * Message catalog shared by the Pi and both STM32s (Lab 3 template, Part 2).
 * Standard 11-bit IDs, DLC 8, every frame sent on both buses.
 * Byte 0 of every payload: 4-bit sequence counter (low nibble) + 4 flag bits.
 */
#ifndef CAN_MSG_H
#define CAN_MSG_H

#include <stdbool.h>
#include <stdint.h>

#define ID_CTRL  0x010
#define ID_FAULT 0x020
#define ID_HB_N1 0x100
#define ID_HB_N2 0x101
#define ID_HB_N3 0x102
#define ID_BLINK 0x200
#define ID_FORCE 0x300

#define MSG_DLC   8
#define MSG_COUNT 7

#define SEQ(d) ((d)[0] & 0x0F)
#define FLAG_RELAYED 0x10 /* byte 0 flag: this copy was forwarded by a relay; never forward it again */

enum { ZONE_N1 = 1, ZONE_N2 = 2, ZONE_N3 = 3 };
enum { BLINK_OFF, BLINK_LEFT, BLINK_RIGHT, BLINK_HAZARD };
enum { STATE_NORMAL, STATE_ERROR };
enum { FAULT_HB_LOST = 1, FAULT_BAD_CTRL = 2, FAULT_TEST_BTN = 3, FAULT_CTRL_LOST = 4 };

#define HEALTH_A_DEG 0x01
#define HEALTH_B_DEG 0x02

/* team-defined ranges: a CTRL outside these puts the receiving zone in error */
#define STEER_MIN    (-450) /* degrees, wheel is 900 deg lock to lock */
#define STEER_MAX    450
#define PEDAL_MAX    100    /* percent */
#define BTN_LEFT     0x01
#define BTN_RIGHT    0x02
#define BTN_TEST     0x04
#define BTN_RESERVED 0xF8

/*
 * Payloads (byte 0 is always seq/flags):
 *   CTRL   b1-2 steer int16 LE | b3 throttle % | b4 brake % | b5 buttons | b6 CRC-8 of b1..b5
 *   FAULT  b1 failing zone | b2 reason | b3 zone that sent it
 *   HB     b1 zone state | b2 health bits (bus A/B degraded)
 *   BLINK  b1 state (off/L/R/hazard) | b2 phase (1 = lamps on)
 *   FORCE  b1 force int8 (dummy counter this lab)
 */
struct ctrl {
	int16_t steer;
	uint8_t throttle;
	uint8_t brake;
	uint8_t buttons;
};

/* CRC-8, polynomial 0x07 */
static inline uint8_t crc8(const uint8_t *p, int n)
{
	uint8_t crc = 0;

	while (n--) {
		crc ^= *p++;
		for (int i = 0; i < 8; i++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static inline void ctrl_encode(const struct ctrl *c, uint8_t d[MSG_DLC])
{
	d[1] = (uint8_t)c->steer;
	d[2] = (uint8_t)((uint16_t)c->steer >> 8);
	d[3] = c->throttle;
	d[4] = c->brake;
	d[5] = c->buttons;
	d[6] = crc8(&d[1], 5);
}

/* false if the frame is corrupt or out of range */
static inline bool ctrl_decode(const uint8_t d[MSG_DLC], struct ctrl *c)
{
	c->steer = (int16_t)(d[1] | (d[2] << 8));
	c->throttle = d[3];
	c->brake = d[4];
	c->buttons = d[5];

	return d[6] == crc8(&d[1], 5) &&
	       c->steer >= STEER_MIN && c->steer <= STEER_MAX &&
	       c->throttle <= PEDAL_MAX && c->brake <= PEDAL_MAX &&
	       !(c->buttons & BTN_RESERVED);
}

static inline int msg_index(uint16_t id)
{
	switch (id) {
	case ID_CTRL:  return 0;
	case ID_FAULT: return 1;
	case ID_HB_N1: return 2;
	case ID_HB_N2: return 3;
	case ID_HB_N3: return 4;
	case ID_BLINK: return 5;
	case ID_FORCE: return 6;
	default:       return -1;
	}
}

/* which zone sent the frame; FAULT names its sender in byte 3 */
static inline int msg_sender(uint16_t id, const uint8_t d[MSG_DLC])
{
	switch (id) {
	case ID_CTRL:
	case ID_BLINK:
	case ID_HB_N1:
		return ZONE_N1;
	case ID_HB_N2:
	case ID_FORCE:
		return ZONE_N2;
	case ID_HB_N3:
		return ZONE_N3;
	case ID_FAULT:
		return d[3];
	default:
		return 0;
	}
}

static inline bool is_hb(uint16_t id)
{
	return id == ID_HB_N1 || id == ID_HB_N2 || id == ID_HB_N3;
}

/*
 * Duplicate filter, one entry per message ID. A frame is the second copy if the same seq
 * was already accepted from the other bus. The same seq again on the same bus is a new
 * frame (the 4-bit counter wrapped), not a duplicate.
 */
struct dedup {
	uint8_t seq;
	uint8_t buses;
	bool valid;
};

static inline bool dedup_is_copy(struct dedup *e, uint8_t seq, int bus)
{
	uint8_t bit = (uint8_t)(1u << bus);

	if (e->valid && e->seq == seq && !(e->buses & bit)) {
		e->buses |= bit;
		return true;
	}
	e->valid = true;
	e->seq = seq;
	e->buses = bit;
	return false;
}

#endif /* CAN_MSG_H */
