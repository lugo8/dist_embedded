/*
 * Message catalog shared by the Pi and both STM32s.
 * Lab 3: CTRL/FAULT/HB/BLINK/FORCE.  Lab 5: adds Raft (REF, VOTE, APPEND, CLIENT) below and
 * folds BLINK's turn state into CTRL b7.
 * Standard 11-bit IDs, DLC 8, every frame sent on both buses.
 * Byte 0 of every payload: 4-bit sequence counter (low nibble) + 4 flag bits.
 *
 * ID map, lowest ID wins arbitration:
 *   0x010        CTRL           N1        brake path, stays highest (R4.4); b7 = turn state
 *   0x020        FAULT          any       error entry
 *   0x030-0x032  REF_Nx         leader    Raft leader heartbeat + time reference, 20 ms
 *   0x040-0x042  VOTE_REQ_Nx    candidate RequestVote
 *   0x048-0x04A  VOTE_RSP_Nx    any       vote reply
 *   0x050-0x052  APPEND_Nx      leader    AppendEntries, one entry per frame (empty = 100 ms)
 *   0x058-0x05A  APPEND_ACK_Nx  follower  AppendEntries reply
 *   0x060-0x062  CLIENT_REQ_Nx  client    report an event to the leader
 *   0x068-0x06A  CLIENT_RSP_Nx  agent     committed / redirect
 *   0x100-0x102  HB_Nx          each zone 20 ms (unchanged, still the R4.5 failure detector)
 *   0x200        BLINK          N1        LAB 3 ONLY, being removed: the state is in CTRL b7 and
 *                                         the phase comes from global time. Kept defined until
 *                                         cockpit.c and blinker.c switch over.
 *   0x300        FORCE          N2        50 ms
 *
 * Every Raft message is a family of three IDs, base + (sender - 1). Different nodes must
 * never send the same ID: CAN arbitration assumes one sender per ID, and two candidates
 * sending REQ_VOTE with the same ID but different data would collide in the data field
 * (bit errors, error frames, possibly bus-off). Per-sender IDs also keep the per-ID dedup
 * table below working unchanged, and give msg_sender() the zone without reading the payload.
 *
 * REF sits above the Raft traffic and the heartbeats because its queuing jitter is clock
 * error, and above VOTE so a live leader's REF suppresses an election that is about to
 * start. It stays below CTRL/FAULT so the brake path is never delayed by more than one frame.
 *
 * Bus load per bus, 8-byte standard frame worst case 135 bits = 270 us at 500 kbit/s:
 *   steady   CTRL 50 + HB 150 + REF 50 + FORCE 20 + APPEND 10 + ACK 20
 *            = 300 frames/s = 8.1 %
 *   catch-up APPEND limited to one per 5 ms (200/s) + 2 ACK each (400/s) + CLIENT 40/s
 *            = 910 frames/s = 24.6 %
 *   + a relay copying everything the leader sends onto the other bus: 34.6 %  (< 50 %, R4.6)
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

/* Lab 5 Raft families: ID = base + (sender zone - 1) */
#define ID_REF_BASE        0x030
#define ID_VOTE_REQ_BASE   0x040
#define ID_VOTE_RSP_BASE   0x048
#define ID_APPEND_BASE     0x050
#define ID_APPEND_ACK_BASE 0x058
#define ID_CLIENT_REQ_BASE 0x060
#define ID_CLIENT_RSP_BASE 0x068

#define RAFT_ID(base, zone) ((uint16_t)((base) + (zone) - 1))
#define ID_REF(z)        RAFT_ID(ID_REF_BASE, z)
#define ID_VOTE_REQ(z)   RAFT_ID(ID_VOTE_REQ_BASE, z)
#define ID_VOTE_RSP(z)   RAFT_ID(ID_VOTE_RSP_BASE, z)
#define ID_APPEND(z)     RAFT_ID(ID_APPEND_BASE, z)
#define ID_APPEND_ACK(z) RAFT_ID(ID_APPEND_ACK_BASE, z)
#define ID_CLIENT_REQ(z) RAFT_ID(ID_CLIENT_REQ_BASE, z)
#define ID_CLIENT_RSP(z) RAFT_ID(ID_CLIENT_RSP_BASE, z)

#define RAFT_FAMILIES 7
#define MSG_DLC   8
#define MSG_COUNT (7 + RAFT_FAMILIES * 3) /* dedup table size: Lab 3 IDs + 3 per Raft family */

/*
 * Receive filters as (id, mask) pairs, for can_add_rx_filter() on the STM32s and
 * CAN_RAW_FILTER on the Pi: 4 filters instead of one per ID (the MCP2515 driver allows 16).
 * 0x000/0x780 passes 0x000-0x07F: CTRL, FAULT and every Raft family.
 * Use as:  #define X(id, mask) { id, mask },  ... CAN_RX_FILTERS(X) ...
 */
#define CAN_RX_FILTERS(X)    \
	X(0x000, 0x780)      \
	X(ID_HB_N1, 0x7FC)   \
	X(ID_BLINK, 0x7FF)   \
	X(ID_FORCE, 0x7FF)

#define SEQ(d) ((d)[0] & 0x0F)
#define FLAG_RELAYED 0x10 /* byte 0 flag: this copy was forwarded by a relay; never forward it again */
#define FLAG_REF_FOLLOWUP 0x20 /* REF only: time_us is the end of the previous REF on this bus */

enum { ZONE_N1 = 1, ZONE_N2 = 2, ZONE_N3 = 3 };
#define ZONE_NONE 0 /* "no leader known" in leader fields */
enum { BLINK_OFF, BLINK_LEFT, BLINK_RIGHT, BLINK_HAZARD };
enum { STATE_NORMAL, STATE_ERROR };
enum { FAULT_HB_LOST = 1, FAULT_BAD_CTRL = 2, FAULT_TEST_BTN = 3, FAULT_CTRL_LOST = 4 };
enum { BUS_A = 0, BUS_B = 1 };

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
 *   CTRL   b1-2 steer int16 LE | b3 throttle % | b4 brake % | b5 buttons
 *          | b6 CRC-8 of b1..b5 and b7 | b7 bits 0-1 turn state (BLINK_OFF/LEFT/RIGHT/HAZARD),
 *          bits 2-7 turn start slot: the 50 ms grid point of global time where a LEFT/RIGHT
 *          signal starts, as (global us / 50000) mod 64. The Pi picks it, so N2 and N3 start
 *          on the same point however late each hears the frame (a relayed copy is ~1 ms late)
 *   FAULT  b1 failing zone | b2 reason | b3 zone that sent it
 *   HB     b1 zone state | b2 health bits (bus A/B degraded) | b3 sync bits (Lab 5)
 *   BLINK  Lab 3 only: b1 state (off/L/R/hazard) | b2 phase (1 = lamps on)
 *   FORCE  b1 force int8 (dummy counter this lab)
 *
 * Raft payloads, Lab 5. Every b1 is the sender's term (see "terms on the wire" below).
 *   REF         b1 term | b2 cycle | b3-6 master time us, bits 0-31 LE
 *               | b7 bit 0 failure mode, bits 1-7 = master time bits 32-38
 *   VOTE_REQ    b1 term | b2-3 last log index u16 LE | b4 last log term
 *   VOTE_RSP    b1 term | b2 candidate | b3 granted (0/1)
 *   APPEND      b1 term | b2-3 index u16 LE | b4 prev term | b5 entry term | b6 event
 *               | b7 commit lag
 *   APPEND_ACK  b1 term | b2 success (0/1) | b3-4 match index u16 LE
 *   CLIENT_REQ  b1 term | b2 node addressed | b3 request id | b4 event
 *   CLIENT_RSP  b1 term | b2 client node | b3 request id | b4 status | b5 leader hint
 *               | b6-7 log index u16 LE
 */
struct ctrl {
	int16_t steer;
	uint8_t throttle;
	uint8_t brake;
	uint8_t buttons;
	uint8_t blink;     /* BLINK_OFF/LEFT/RIGHT/HAZARD, after the cockpit's turn-cancel logic */
	uint8_t turn_slot; /* 0..63: where a LEFT/RIGHT signal starts (b7 bits 2-7) */
};

#define TURN_GRID_US 50000 /* the turn start slot counts this grid of global time */

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

/* CTRL's CRC covers b1..b5 and b7 (b6 is the CRC itself) */
static inline uint8_t ctrl_crc(const uint8_t d[MSG_DLC])
{
	const uint8_t covered[6] = { d[1], d[2], d[3], d[4], d[5], d[7] };

	return crc8(covered, 6);
}

static inline void ctrl_encode(const struct ctrl *c, uint8_t d[MSG_DLC])
{
	d[1] = (uint8_t)c->steer;
	d[2] = (uint8_t)((uint16_t)c->steer >> 8);
	d[3] = c->throttle;
	d[4] = c->brake;
	d[5] = c->buttons;
	d[7] = (uint8_t)((c->blink & 0x03) | (c->turn_slot << 2));
	d[6] = ctrl_crc(d);
}

/* false if the frame is corrupt or out of range */
static inline bool ctrl_decode(const uint8_t d[MSG_DLC], struct ctrl *c)
{
	c->steer = (int16_t)(d[1] | (d[2] << 8));
	c->throttle = d[3];
	c->brake = d[4];
	c->buttons = d[5];
	c->blink = d[7] & 0x03;
	c->turn_slot = d[7] >> 2;

	return d[6] == ctrl_crc(d) &&
	       c->steer >= STEER_MIN && c->steer <= STEER_MAX &&
	       c->throttle <= PEDAL_MAX && c->brake <= PEDAL_MAX &&
	       !(c->buttons & BTN_RESERVED);
}

/* ------------------------------------------------------------------------------------ */
/* Lab 5: Raft                                                                          */
/* ------------------------------------------------------------------------------------ */

/*
 * Log events (handout Part 3), one byte. High nibble = kind, low nibble = argument.
 * The checkoff calls NODE_* and CANLINK_* entries "MEMBER" entries; COCKPIT_* and the
 * team-defined EV_APP range are "EVENT" entries.
 *   0x00       none (empty APPEND, no entry)
 *   0x01/0x02  COCKPIT_OK / COCKPIT_ERROR
 *   0x05-0x07  LEADER_X: X won an election (Raft no-op entry, lets it commit older entries)
 *   0x1X/0x2X  NODE_X_UP / NODE_X_DOWN                    X = 1..3
 *   0x3L/0x4L  CANLINK_X_Y_UP / CANLINK_X_Y_DOWN          L = (X - 1) * 2 + bus, 0..5
 *   0x80-0xFF  EV_APP(n), team-defined application events: test button, blinker on/off,
 *              bad CTRL, each tagged with the node that saw it (EV_APP_MAKE)
 */
#define EV_NONE          0x00
#define EV_COCKPIT_OK    0x01
#define EV_COCKPIT_ERROR 0x02
#define EV_LEADER(x)     ((uint8_t)(0x04 | (x)))
#define EV_NODE_UP(x)    ((uint8_t)(0x10 | (x)))
#define EV_NODE_DOWN(x)  ((uint8_t)(0x20 | (x)))
#define EV_LINK_UP(x, bus)   ((uint8_t)(0x30 | (((x) - 1) << 1) | (bus)))
#define EV_LINK_DOWN(x, bus) ((uint8_t)(0x40 | (((x) - 1) << 1) | (bus)))
#define EV_APP(n)        ((uint8_t)(0x80 | (n)))

/* the team's application events (EV_APP range): 0x80 | kind << 3 | node */
enum { APP_TEST_BTN = 1, APP_LEFT_ON = 2, APP_RIGHT_ON = 3, APP_HAZARD_ON = 4, APP_BLINK_OFF = 5,
       APP_BAD_CTRL = 6 };
#define EV_APP_MAKE(kind, node) ((uint8_t)(0x80 | ((kind) << 3) | (node)))
#define EV_APP_KIND(e)          (((e) >> 3) & 0x0F)
#define EV_APP_NODE(e)          ((e) & 0x07)

enum { EVK_COCKPIT = 0x00, EVK_NODE_UP = 0x10, EVK_NODE_DOWN = 0x20,
       EVK_LINK_UP = 0x30, EVK_LINK_DOWN = 0x40, EVK_APP = 0x80 };

static inline uint8_t ev_kind(uint8_t e)
{
	return (e & 0x80) ? EVK_APP : (uint8_t)(e & 0xF0);
}

/* node X for NODE_*, CANLINK_* and LEADER_*, 0 otherwise */
static inline int ev_node(uint8_t e)
{
	switch (ev_kind(e)) {
	case EVK_COCKPIT:
		return (e & 0x04) ? (e & 0x03) : 0;
	case EVK_NODE_UP:
	case EVK_NODE_DOWN:
		return e & 0x0F;
	case EVK_LINK_UP:
	case EVK_LINK_DOWN:
		return ((e & 0x0F) >> 1) + 1;
	default:
		return 0;
	}
}

/* bus Y for CANLINK_* (BUS_A / BUS_B), -1 otherwise */
static inline int ev_bus(uint8_t e)
{
	uint8_t k = ev_kind(e);

	return (k == EVK_LINK_UP || k == EVK_LINK_DOWN) ? (e & 1) : -1;
}

/* true for a code a log entry may hold (EV_NONE is not an entry) */
static inline bool ev_valid(uint8_t e)
{
	switch (ev_kind(e)) {
	case EVK_COCKPIT:
		return e == EV_COCKPIT_OK || e == EV_COCKPIT_ERROR ||
		       (e >= EV_LEADER(ZONE_N1) && e <= EV_LEADER(ZONE_N3));
	case EVK_NODE_UP:
	case EVK_NODE_DOWN:
		return (e & 0x0F) >= ZONE_N1 && (e & 0x0F) <= ZONE_N3;
	case EVK_LINK_UP:
	case EVK_LINK_DOWN:
		return (e & 0x0F) <= 5;
	case EVK_APP:
		return true;
	default:
		return false;
	}
}

enum { ROLE_FOLLOWER, ROLE_CANDIDATE, ROLE_LEADER };

/* REF b7 bit 0; bits 1-7 carry master time bits 32-38 */
#define REF_MODE_FAILURE 0x01 /* leader has applied COCKPIT_ERROR: failure mode, 4 Hz hazards */
#define REF_TIME_BITS    39   /* master time on the wire: 2^39 us = 6.4 days of uptime */

/* HB b3 sync bits */
#define SYNC_LOCKED 0x01 /* clock follows a REF heard in the last missed_heartbeats periods */
#define SYNC_DRIFT  0x02 /* a drift ratio has been measured (two REFs paired) */

/* CLIENT_RSP b4 status */
enum { CL_OK = 0,        /* entry committed at index b6-7 (or nothing to log: state already so) */
       CL_REDIRECT = 1,  /* not leader; b5 names the leader, ZONE_NONE if unknown */
       CL_BUSY = 2 };    /* leader log full / election in progress: retry */

/*
 * Terms on the wire are the low 8 bits. The receiver extends a wire term to the full value
 * as the one closest to a reference it already has: its own current term for b1, and the
 * frame's (extended) b1 term for prev/entry/last-log terms, which are never newer than it.
 * Correct while the two differ by less than 128, i.e. fewer than 128 elections while a
 * node was away. Keeps APPEND to one frame per entry with byte-aligned fields.
 */
static inline uint32_t term_unwire(uint8_t w, uint32_t ref)
{
	int64_t t = (int64_t)ref + (int8_t)(uint8_t)(w - (uint8_t)ref);

	return t < 0 ? (uint32_t)(t + 256) : (uint32_t)t;
}

static inline void put16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t get16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static inline void put32(uint8_t *p, uint32_t v)
{
	put16(p, (uint16_t)v);
	put16(p + 2, (uint16_t)(v >> 16));
}

static inline uint32_t get32(const uint8_t *p)
{
	return get16(p) | ((uint32_t)get16(p + 2) << 16);
}

static inline bool zone_ok(uint8_t z)
{
	return z >= ZONE_N1 && z <= ZONE_N3;
}

/*
 * REF, leader -> all, every heartbeat_ms (20 ms). It is both the Raft heartbeat that resets
 * follower election timers and the time reference (Part 6).
 * cycle goes up by one per REF so a follower can pair consecutive REFs (and tell a missed
 * one). time_us is the master's global time in microseconds, 39 bits (b3-6 plus b7 bits
 * 1-7), so 6.4 days before it wraps. A first version sent 32 bits (71 min) and had the
 * receiver guess the rest from its own clock: a follower that had just been reset (clock
 * near 0) against a Pi up for over 36 min guessed wrong, landed 2^32 us behind, in negative
 * time, and the blinkers and PPS went wrong.
 * FLAG_REF_FOLLOWUP clear: time_us was read when the frame was queued.
 * FLAG_REF_FOLLOWUP set:   time_us is when the previous REF (cycle - 1) finished on this
 *   bus (TX-done callback). Pair it with the receive time of that REF on the same bus: every
 *   node sees the end of a frame at the same instant, so the only delay left is interrupt
 *   latency, not queuing. Each bus copy then carries its own bus's time (same seq on both).
 */
struct raft_ref {
	uint32_t term;
	uint8_t cycle;
	uint64_t time_us; /* REF_TIME_BITS on the wire */
	uint8_t mode;     /* REF_MODE_FAILURE */
	bool followup;
};

static inline void ref_encode(const struct raft_ref *m, uint8_t d[MSG_DLC])
{
	d[0] = m->followup ? FLAG_REF_FOLLOWUP : 0;
	d[1] = (uint8_t)m->term;
	d[2] = m->cycle;
	put32(&d[3], (uint32_t)m->time_us);
	d[7] = (uint8_t)((m->mode & REF_MODE_FAILURE) | (((m->time_us >> 32) & 0x7F) << 1));
}

static inline bool ref_decode(const uint8_t d[MSG_DLC], uint32_t my_term, struct raft_ref *m)
{
	m->term = term_unwire(d[1], my_term);
	m->cycle = d[2];
	m->time_us = get32(&d[3]) | ((uint64_t)(d[7] >> 1) << 32);
	m->mode = d[7] & REF_MODE_FAILURE;
	m->followup = d[0] & FLAG_REF_FOLLOWUP;
	return true;
}

/* VOTE_REQ, candidate -> all */
struct raft_vote_req {
	uint32_t term;
	uint16_t last_index;
	uint32_t last_term;
};

static inline void vote_req_encode(const struct raft_vote_req *m, uint8_t d[MSG_DLC])
{
	d[0] = 0;
	d[1] = (uint8_t)m->term;
	put16(&d[2], m->last_index);
	d[4] = (uint8_t)m->last_term;
	d[5] = d[6] = d[7] = 0;
}

static inline bool vote_req_decode(const uint8_t d[MSG_DLC], uint32_t my_term,
				   struct raft_vote_req *m)
{
	m->term = term_unwire(d[1], my_term);
	m->last_index = get16(&d[2]);
	m->last_term = term_unwire(d[4], m->term);
	return m->last_term <= m->term && !(d[5] | d[6] | d[7]);
}

/* VOTE_RSP, any -> candidate (broadcast; only the named candidate counts it) */
struct raft_vote_rsp {
	uint32_t term;
	uint8_t candidate;
	bool granted;
};

static inline void vote_rsp_encode(const struct raft_vote_rsp *m, uint8_t d[MSG_DLC])
{
	d[0] = 0;
	d[1] = (uint8_t)m->term;
	d[2] = m->candidate;
	d[3] = m->granted;
	d[4] = d[5] = d[6] = d[7] = 0;
}

static inline bool vote_rsp_decode(const uint8_t d[MSG_DLC], uint32_t my_term,
				   struct raft_vote_rsp *m)
{
	m->term = term_unwire(d[1], my_term);
	m->candidate = d[2];
	m->granted = d[3];
	return zone_ok(m->candidate) && d[3] <= 1 && !(d[4] | d[5] | d[6] | d[7]);
}

/*
 * APPEND, leader -> all. One entry per frame, AppendEntries with prevLogIndex = index - 1.
 *   event != EV_NONE: store (index, entry_term, event) if the entry at index - 1 has
 *                     prev_term (index 1: prev is the empty log, term 0, always matches).
 *   event == EV_NONE: empty heartbeat, sent every 100 ms and right after a commit;
 *                     index = leader's last index + 1, nothing is stored, the prev check
 *                     and the commit still apply.
 * commit_lag: the leader's commit index, as index - commit, clipped to 0..255. It is
 *   min(leaderCommit, index) on the wire, which is all Raft lets a follower use from this
 *   frame anyway; a follower far behind just commits up to index - 255 for now.
 * Broadcast, so caught-up followers also see catch-up frames for others: an entry they
 * already hold with the same term is acked and otherwise ignored.
 */
struct raft_append {
	uint32_t term;
	uint16_t index;
	uint32_t prev_term;
	uint32_t entry_term; /* unused (0) when event == EV_NONE */
	uint8_t event;
	uint16_t commit;     /* leader commit index; encode clips it, decode reconstructs */
};

static inline void append_encode(const struct raft_append *m, uint8_t d[MSG_DLC])
{
	uint16_t c = m->commit < m->index ? m->commit : m->index;
	uint16_t lag = (uint16_t)(m->index - c);

	d[0] = 0;
	d[1] = (uint8_t)m->term;
	put16(&d[2], m->index);
	d[4] = (uint8_t)m->prev_term;
	d[5] = m->event == EV_NONE ? 0 : (uint8_t)m->entry_term;
	d[6] = m->event;
	d[7] = lag > 255 ? 255 : (uint8_t)lag;
}

static inline bool append_decode(const uint8_t d[MSG_DLC], uint32_t my_term,
				 struct raft_append *m)
{
	m->term = term_unwire(d[1], my_term);
	m->index = get16(&d[2]);
	m->prev_term = term_unwire(d[4], m->term);
	m->event = d[6];
	m->entry_term = m->event == EV_NONE ? 0 : term_unwire(d[5], m->term);
	m->commit = (uint16_t)(m->index - d[7]);
	return m->index >= 1 && d[7] <= m->index && m->prev_term <= m->term &&
	       m->entry_term <= m->term && (m->event == EV_NONE || ev_valid(m->event)) &&
	       !(m->event == EV_NONE && d[5]);
}

/*
 * APPEND_ACK, follower -> leader. Broadcast; the term alone says which leader it answers,
 * since a term has at most one leader, so a stale leader ignores it by term.
 *   success: match is the highest index now known to match the leader.
 *   failure: match is the follower's last index, so the leader can back up nextIndex to
 *            min(nextIndex - 1, match + 1) at once instead of one step per round.
 */
struct raft_ack {
	uint32_t term;
	bool success;
	uint16_t match;
};

static inline void ack_encode(const struct raft_ack *m, uint8_t d[MSG_DLC])
{
	d[0] = 0;
	d[1] = (uint8_t)m->term;
	d[2] = m->success;
	put16(&d[3], m->match);
	d[5] = d[6] = d[7] = 0;
}

static inline bool ack_decode(const uint8_t d[MSG_DLC], uint32_t my_term, struct raft_ack *m)
{
	m->term = term_unwire(d[1], my_term);
	m->success = d[2];
	m->match = get16(&d[3]);
	return d[2] <= 1 && !(d[5] | d[6] | d[7]);
}

/*
 * CLIENT_REQ, client -> the node it believes is leader (handout Part 1, Figure 2).
 * A client on the leader's own node calls its agent directly; this frame is for clients on
 * other nodes. req_id goes up per request so the leader can drop a retry it already logged.
 * The leader also drops reports that change nothing (link already down in the state machine,
 * counting uncommitted entries), so two nodes reporting the same fault give one MEMBER entry.
 */
struct raft_client_req {
	uint32_t term;
	uint8_t dst;
	uint8_t req_id;
	uint8_t event;
};

static inline void client_req_encode(const struct raft_client_req *m, uint8_t d[MSG_DLC])
{
	d[0] = 0;
	d[1] = (uint8_t)m->term;
	d[2] = m->dst;
	d[3] = m->req_id;
	d[4] = m->event;
	d[5] = d[6] = d[7] = 0;
}

static inline bool client_req_decode(const uint8_t d[MSG_DLC], uint32_t my_term,
				     struct raft_client_req *m)
{
	m->term = term_unwire(d[1], my_term);
	m->dst = d[2];
	m->req_id = d[3];
	m->event = d[4];
	return zone_ok(m->dst) && ev_valid(m->event) && !(d[5] | d[6] | d[7]);
}

/* CLIENT_RSP, agent -> client: CL_OK once committed, CL_REDIRECT from a follower */
struct raft_client_rsp {
	uint32_t term;
	uint8_t client;
	uint8_t req_id;
	uint8_t status;
	uint8_t leader; /* ZONE_NONE if unknown */
	uint16_t index; /* CL_OK: where the entry was committed, 0 if nothing was logged */
};

static inline void client_rsp_encode(const struct raft_client_rsp *m, uint8_t d[MSG_DLC])
{
	d[0] = 0;
	d[1] = (uint8_t)m->term;
	d[2] = m->client;
	d[3] = m->req_id;
	d[4] = m->status;
	d[5] = m->leader;
	put16(&d[6], m->index);
}

static inline bool client_rsp_decode(const uint8_t d[MSG_DLC], uint32_t my_term,
				     struct raft_client_rsp *m)
{
	m->term = term_unwire(d[1], my_term);
	m->client = d[2];
	m->req_id = d[3];
	m->status = d[4];
	m->leader = d[5];
	m->index = get16(&d[6]);
	return zone_ok(m->client) && m->status <= CL_BUSY &&
	       (m->leader == ZONE_NONE || zone_ok(m->leader));
}

/* ------------------------------------------------------------------------------------ */
/* ID helpers, dedup                                                                    */
/* ------------------------------------------------------------------------------------ */

/* 0..RAFT_FAMILIES-1 for a Raft ID with a valid sender, else -1 */
static inline int raft_family(uint16_t id)
{
	if ((id & 0x7) > 2) {
		return -1;
	}
	switch (id & 0x7F8) {
	case ID_REF_BASE:        return 0;
	case ID_VOTE_REQ_BASE:   return 1;
	case ID_VOTE_RSP_BASE:   return 2;
	case ID_APPEND_BASE:     return 3;
	case ID_APPEND_ACK_BASE: return 4;
	case ID_CLIENT_REQ_BASE: return 5;
	case ID_CLIENT_RSP_BASE: return 6;
	default:                 return -1;
	}
}

static inline bool is_raft(uint16_t id)
{
	return raft_family(id) >= 0;
}

/* the family base of a Raft ID (ID_REF_BASE, ...), for switch statements */
static inline uint16_t raft_base(uint16_t id)
{
	return (uint16_t)(id & 0x7F8);
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
	default: {
		int fam = raft_family(id);

		return fam < 0 ? -1 : 7 + fam * 3 + (id & 0x7);
	}
	}
}

/* which zone sent the frame; FAULT names its sender in byte 3, Raft IDs in their low bits */
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
		return is_raft(id) ? (id & 0x7) + 1 : 0;
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
 * REF: run the clock-sync step on each bus copy before this filter (followup times are
 * per bus); only the Raft heartbeat handling needs to see one copy.
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
