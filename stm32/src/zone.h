/* shared by both STM32 zones; ZONE_FRONT picks N2 (steering), otherwise N3 (drivetrain) */
#ifndef ZONE_H
#define ZONE_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include "can_msg.h"

#ifdef ZONE_FRONT
#define MY_ZONE   ZONE_N2
#define MY_HB     ID_HB_N2
#define PEER_ZONE ZONE_N3
#else
#define MY_ZONE   ZONE_N3
#define MY_HB     ID_HB_N3
#define PEER_ZONE ZONE_N2
#endif

/* thresholds, all in one place */
#define HB_PERIOD_MS    20
#define HB_TIMEOUT_MS   65   /* 3 missed heartbeats + margin (R4.5) */
#define RELAY_DETECT_MS 30   /* relay once a zone misses one heartbeat on a bus: well under
				HB_TIMEOUT_MS, so relayed heartbeats arrive before anyone
				declares that zone failed */
#define CTRL_TIMEOUT_MS 250  /* no valid CTRL this long: brake and go to error; wifi can
				stall the wheel stream for 100+ ms */
#define DEG_TIMEOUT_MS  45   /* DEG on within 60 ms of a pull (R4.7): 45 + one 10 ms check */
#define MON_PERIOD_MS   10
#define BAD_CMD_HOLD_MS 1000 /* stay in error this long after an out-of-range CTRL */
#define BLINK_HALF_MS   500  /* 1 Hz (R2.4) */
#define HAZARD_HALF_MS  250  /* 2 Hz hazard pattern (S4, via R3) */
#define BLINK_STALE_MS  1000 /* no BLINK for this long: the cockpit is gone */
#define DEBOUNCE_MS     50   /* presses closer than 50 ms count as one (R2.5) */
#define DOUBLE_PRESS_MS 500
#define WDT_TIMEOUT_MS  100
#define FORCE_PERIOD_MS 50
#define PID_PERIOD_MS   10

/*
 * Fixed priorities, deadline-monotonic (lower number = higher priority).
 * The MCP2515 driver thread is 2 (CONFIG_CAN_MCP2515_INT_THREAD_PRIO), above all of these.
 */
#define PRIO_RELAY    3 /* on the brake path whenever a relay is active */
#define PRIO_DISPATCH 4 /* CAN_RX, 0.3 ms */
#define PRIO_ACT      5 /* DRIVE / STEER / BLINK, 0.5-1 ms */
#define PRIO_BTN      6 /* TEST_BTN, 10 ms */
#define PRIO_PID      6 /* PID, 10 ms */
#define PRIO_HB       7 /* HB_TX and HB_MON, 20 ms */
#define PRIO_FORCE    8 /* FORCE_TX, 50 ms */

/* can_io.c */
int can_io_init(void);
void send_both(uint16_t id, atomic_t *seq, uint8_t d[MSG_DLC]);

/* monitor.c */
int monitor_init(void);
void monitor_rx(int bus, int zone, bool hb, bool relayed);
void monitor_ctrl(bool valid);
bool monitor_in_error(void);
bool monitor_self_test(void);
bool monitor_hb_on_bus(int zone, int bus);
bool monitor_bus_degraded(int bus);

/* blinker.c */
int blinker_init(void);
void blinker_msg(uint8_t state, uint8_t phase);

/* front.c (STEER, FORCE_TX) or rear.c (DRIVE, PID) */
int actuator_init(void);
void actuator_ctrl(const struct ctrl *c);
void actuator_error(void);

#endif /* ZONE_H */
