/* shared by both STM32 zones; ZONE_FRONT picks N2 (steering), otherwise N3 (drivetrain) */
#ifndef ZONE_H
#define ZONE_H

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include "can_msg.h"
#include "raft.h"

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
#define FAILURE_HALF_MS 125  /* 4 Hz hazards in failure mode (Lab 5 checkoff 6) */
#define BLINK_GRID_MS   50   /* lamps change only on this global-time grid */
#define BLINK_STALE_MS  1000 /* no CTRL for this long: the cockpit is gone */
#define RAFT_TICK_MS    2
#define STATUS_PRINT_MS 5000 /* leader + Part 6.2 drift printout */
#define DEBOUNCE_MS     50   /* presses closer than 50 ms count as one (R2.5) */
#define DOUBLE_PRESS_MS 500
#define WDT_TIMEOUT_MS  100
#define FORCE_PERIOD_MS 50
#define PID_PERIOD_MS   10

/*
 * Fixed priorities, deadline-monotonic (lower number = higher priority).
 * The MCP2515 driver thread is 2 (CONFIG_CAN_MCP2515_INT_THREAD_PRIO), above all of these.
 */
#define PRIO_PPS      3 /* PPS edge: wakes once a second, busy-waits < 200 us */
#define PRIO_RELAY    3 /* on the brake path whenever a relay is active */
#define PRIO_DISPATCH 4 /* CAN_RX, 0.3 ms */
#define PRIO_ACT      5 /* DRIVE / STEER / BLINK, 0.5-1 ms */
#define PRIO_BTN      6 /* TEST_BTN, 10 ms */
#define PRIO_PID      6 /* PID, 10 ms */
#define PRIO_REF      6 /* REF_TX, 20 ms; its time comes from TX-done, so its jitter is harmless */
#define PRIO_RAFT     7 /* RAFT agent, 2 ms tick: never on the brake path */
#define PRIO_HB       7 /* HB_TX and HB_MON, 20 ms */
#define PRIO_FORCE    8 /* FORCE_TX, 50 ms */
#define PRIO_PRINT    9 /* console printouts (PPS lines, drift, applied entries) */

/* can_io.c */
int can_io_init(void);
void send_both(uint16_t id, atomic_t *seq, uint8_t d[MSG_DLC]);
/* REF: one frame per bus (each carries its own bus's follow-up time), one shared seq;
 * tx_done(bus) is called from the driver thread when that bus's copy is on the wire */
void send_ref(uint16_t id, atomic_t *seq, uint8_t d[2][MSG_DLC], void (*tx_done)(int bus));
/* the relay route_relay() picked, set by the raft agent */
extern atomic_t relay_zone;

/* raft_agent.c: Raft, routing, time sync, LEADER/SYNC LEDs */
struct raft_rx {
	struct can_frame_lite {
		uint16_t id;
		uint8_t data[MSG_DLC];
	} f;
	uint8_t bus;
	bool relayed;
	bool first; /* first copy (passed dedup) */
	int64_t rx_us;
};
int raft_agent_init(void);
void raft_agent_rx(const struct raft_rx *it);   /* every accepted frame, both copies */
void raft_agent_event(uint8_t app_kind);        /* EV_APP event from this node */
uint8_t raft_agent_hb_sync_bits(void);          /* HB b3 */
bool raft_agent_failure_mode(void);             /* committed COCKPIT_ERROR (or REF says so) */
int64_t local_us(void);                         /* this board's clock, us since boot */
int64_t global_gettime(void);                   /* Part 6.3: global time, us */
int64_t global_to_local(int64_t g);             /* inverse, for scheduling an edge */
void agent_printf(const char *fmt, ...);        /* console line from the PRINT thread */
struct timesync;
void raft_agent_ts(struct timesync *out);       /* snapshot of the time sync state */

/* pps.c */
int pps_init(void);

/* monitor.c */
int monitor_init(void);
void monitor_rx(int bus, int zone, bool hb, bool relayed);
void monitor_ctrl(bool valid);
bool monitor_in_error(void);
bool monitor_self_test(void);
bool monitor_hb_on_bus(int zone, int bus);
bool monitor_bus_degraded(int bus);
bool monitor_sending_hb(void);
bool monitor_hb_both(int zone);
uint8_t monitor_deg_bits(void);

/* blinker.c */
int blinker_init(void);
void blinker_ctrl(uint8_t state, uint8_t slot); /* turn state + start slot from CTRL b7 */

/* front.c (STEER, FORCE_TX) or rear.c (DRIVE, PID) */
int actuator_init(void);
void actuator_ctrl(const struct ctrl *c);
void actuator_error(void);

#endif /* ZONE_H */
