/*
 * Raft agent, client and network state machine (Lab 5 Parts 1-5).
 * Plain C, no OS: the same code runs on both STM32s, on the Pi and in tests/raft_sim.c.
 *
 * Threading: nothing in here locks. The platform calls every function with its own lock
 * held (one mutex around the whole struct raft).
 * Time: milliseconds from any free-running clock (k_uptime_get on Zephyr,
 * CLOCK_MONOTONIC on the Pi), as uint32_t; only differences are used, so it may wrap.
 *
 * Platform duties:
 *   - every accepted frame, both bus copies, relayed or not:  raft_heard()
 *   - REF (first copy):                                         raft_on_ref()
 *   - other Raft frames (first copy, after dedup):              raft_on_frame()
 *   - every 1-5 ms:                                             raft_tick()
 *   - every heartbeat_ms while raft_ref_info() says leader:     send REF (with the time)
 *   - application events (test button, blinker, bad CTRL):      raft_report()
 */
#ifndef RAFT_H
#define RAFT_H

#include <stdbool.h>
#include <stdint.h>
#include "can_msg.h"
#include "raft_conf.h"

#ifndef RAFT_TRACK_REPLAY
#define RAFT_TRACK_REPLAY 1 /* keep a per-entry "arrived already committed" bit (log_size / 8 B) */
#endif

#define RAFT_NODES      3
#define RAFT_MAJORITY   (RAFT_CONF_N / 2 + 1)
#define RAFT_TERM_RUNS  64 /* distinct terms the log can hold (one run per election that logged) */
#define RAFT_PENDING    8  /* client requests the leader is waiting to commit */
#define RAFT_APP_QUEUE  16 /* application events a client holds until committed */

/* derived timing; all in ms */
#define RAFT_ELECTION_MIN_MS (RAFT_CONF_MISSED_HEARTBEATS * RAFT_CONF_HEARTBEAT_MS)
#define RAFT_BOOT_LISTEN_MS  (RAFT_ELECTION_MIN_MS + RAFT_CONF_JITTER_MS + 50)
#define RAFT_PEER_WINDOW_MS  50  /* "not isolated": heard another node this recently */
#define RAFT_LEADER_ALONE_MS 100 /* a leader that hears nobody this long steps down */
#define RAFT_APPEND_IDLE_MS  100 /* empty APPEND period (checkoff step 2) */
#define RAFT_APPEND_GAP_MS   5   /* at most one APPEND with an entry per 5 ms (bus load) */
#define RAFT_APPEND_RETRY_MS 30  /* resend an unacknowledged entry */
#define RAFT_FOLLOWER_LIVE_MS 300 /* replicate only to followers heard this recently */
#define RAFT_LINK_TIMEOUT_MS 250 /* own link y is down: no other node heard directly on y */
#define RAFT_NODE_TIMEOUT_MS 300 /* node x is down: nothing at all from x, on any bus */
#define RAFT_CLIENT_RETRY_MS 50
#define RAFT_CLIENT_HOLD_MS  200 /* after an OK, wait this long for our commit to catch up */

enum raft_role { RAFT_BOOT = 3 }; /* plus ROLE_FOLLOWER / CANDIDATE / LEADER from can_msg.h */

/* the network graph the log describes (Part 4); index 1..3 = node */
struct net_state {
	bool node_up[RAFT_NODES + 1];
	bool link_up[RAFT_NODES + 1][2];
	bool cockpit_ok; /* false = failure mode */
};

struct raft;

struct raft_io {
	void *ctx;
	/* queue one frame on both buses; the platform sets the seq nibble */
	void (*send)(void *ctx, uint16_t id, uint8_t d[MSG_DLC]);
	/* an entry was committed and applied to the state machine (in index order) */
	void (*applied)(void *ctx, uint16_t index, uint32_t term, uint8_t event, bool replayed);
	/* optional: term/vote changed (from == 0) or the log changed from index `from` on */
	void (*persist)(void *ctx, const struct raft *r, uint16_t from);
	/* optional debug output, printf-style, no trailing newline needed */
	void (*log)(void *ctx, const char *fmt, ...);
};

struct term_run {
	uint16_t start; /* first index with this term */
	uint32_t term;
};

struct follower {
	uint16_t next;    /* next index to send */
	uint16_t match;   /* highest index known replicated */
	uint32_t sent_ms; /* when `next` was last sent */
	bool inflight;
};

struct pending_rsp {
	uint8_t client;
	uint8_t req_id;
	uint16_t index;
	bool used;
};

struct raft {
	uint8_t me;
	struct raft_io io;

	/* persistent state (Pi: saved through io.persist) */
	uint32_t term;
	uint8_t voted_for;
	uint16_t last;  /* last log index, 0 = empty */
	uint8_t ev[RAFT_CONF_LOG_SIZE + 1]; /* ev[i] = event at index i */
	struct term_run runs[RAFT_TERM_RUNS];
	uint8_t nruns;

	/* volatile */
	uint8_t role;
	uint8_t leader; /* ZONE_NONE if unknown */
	uint16_t commit;
	uint16_t applied_idx;
	struct net_state net;  /* committed + applied: getNetwork() */
	struct net_state tail; /* every entry in the log, committed or not (leader dedup) */
#if RAFT_TRACK_REPLAY
	uint8_t replay[RAFT_CONF_LOG_SIZE / 8 + 1];
#endif

	/* timers */
	uint32_t now;
	uint32_t boot_ms;
	uint32_t leader_seen_ms; /* last valid REF/APPEND from the current leader, or own vote */
	uint32_t election_ms;    /* current timeout (base + jitter) */
	uint32_t rng;
	bool heard_leader_once;
	bool formed;             /* every node heard since boot (election modes) */
	uint32_t formed_ms;
	uint32_t heard_ms[RAFT_NODES + 1];        /* any frame from x, any bus, relayed too */
	uint32_t heard_bus_ms[RAFT_NODES + 1][2]; /* direct frames from x on bus y */
	bool heard_any[RAFT_NODES + 1];

	/* candidate */
	uint8_t votes; /* bitmask of zones */

	/* leader */
	struct follower f[RAFT_NODES + 1];
	uint32_t last_append_ms;  /* last APPEND of any kind */
	uint32_t last_entry_tx_ms;
	bool commit_dirty;        /* push an empty APPEND now: commit moved */
	struct pending_rsp pend[RAFT_PENDING];
	uint8_t last_req[RAFT_NODES + 1];
	uint16_t last_req_idx[RAFT_NODES + 1];
	bool last_req_valid[RAFT_NODES + 1];

	/* client */
	uint8_t req_id;
	bool cl_inflight;
	uint8_t cl_event;        /* what is in flight */
	uint8_t cl_req;
	uint8_t cl_target;
	uint32_t cl_sent_ms;
	bool cl_is_app;
	uint32_t cl_hold_ms[RAFT_NODES + 1][3]; /* [x][0] node item, [x][1..2] link items */
	uint8_t app_q[RAFT_APP_QUEUE];
	uint8_t app_head, app_n;
};

void raft_init(struct raft *r, uint8_t me, const struct raft_io *io, uint32_t now);

/* Pi only, before the first raft_tick: what io.persist saved last run */
void raft_restore(struct raft *r, uint32_t term, uint8_t voted_for);
bool raft_restore_entry(struct raft *r, uint16_t index, uint32_t term, uint8_t event);

void raft_heard(struct raft *r, uint8_t zone, int bus, bool relayed, uint32_t now);
void raft_on_ref(struct raft *r, uint8_t sender, const uint8_t d[MSG_DLC], uint32_t now);
void raft_on_frame(struct raft *r, uint16_t id, const uint8_t d[MSG_DLC], uint32_t now);
void raft_tick(struct raft *r, uint32_t now);
void raft_report(struct raft *r, uint8_t app_event);

/* REF content for the platform's 20 ms REF task; false if this node should not send REF */
bool raft_ref_info(const struct raft *r, uint32_t *term, uint8_t *mode);
/* is this REF from the leader this node follows (use its time)? */
bool raft_ref_is_master(const struct raft *r, uint8_t sender, const uint8_t d[MSG_DLC]);

/* Part 4: the committed network graph */
static inline struct net_state raft_get_network(const struct raft *r)
{
	return r->net;
}

uint32_t raft_entry_term(const struct raft *r, uint16_t index);
bool raft_isolated(const struct raft *r);

/* the PRNG behind the election jitter, exposed for tools/find_seed */
uint32_t raft_rand_init(uint32_t seed, uint8_t me);
uint32_t raft_rand_next(uint32_t *state);
uint32_t raft_first_jitter(uint32_t seed, uint8_t me);

const char *raft_node_name(uint8_t zone); /* "cockpit", "front", "rear", "none" */
const char *raft_role_name(uint8_t role); /* "follower", "candidate", "LEADER", "booting" */

/* "NODE_2_DOWN" etc. into buf; returns buf */
const char *raft_event_name(uint8_t e, char *buf, int len);
void net_state_print(const struct net_state *s, char *buf, int len);

#endif /* RAFT_H */
