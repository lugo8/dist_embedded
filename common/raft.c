/* Raft agent, client and network state machine; see raft.h */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "raft.h"

#define BIT(z) ((uint8_t)(1u << (z)))

static const char *const node_name[] = RAFT_CONF_NODE_NAMES;

#define LOG(r, ...)                                      \
	do {                                             \
		if (RAFT_CONF_DEBUG && (r)->io.log) {    \
			(r)->io.log((r)->io.ctx, __VA_ARGS__); \
		}                                        \
	} while (0)

static uint32_t since(const struct raft *r, uint32_t t)
{
	return r->now - t;
}

/* ---------------------------------------------------------------- PRNG (xorshift32) */

uint32_t raft_rand_init(uint32_t seed, uint8_t me)
{
	/* murmur3 finalizer over (seed, node), so the three nodes' streams are unrelated */
	uint32_t s = seed ^ (uint32_t)me * 0x9E3779B9u;

	s ^= s >> 16;
	s *= 0x85EBCA6Bu;
	s ^= s >> 13;
	s *= 0xC2B2AE35u;
	s ^= s >> 16;
	return s ? s : 0xA5A5A5A5u;
}

uint32_t raft_rand_next(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *s = x;
}

uint32_t raft_first_jitter(uint32_t seed, uint8_t me)
{
	uint32_t s = raft_rand_init(seed, me);

	return raft_rand_next(&s) % (RAFT_CONF_JITTER_MS + 1);
}

/* ---------------------------------------------------------------- network state */

static void net_init(struct net_state *s)
{
	/* the topology of Figure 3, everything up; the log records changes from here */
	memset(s, 0, sizeof(*s));
	for (int x = 1; x <= RAFT_NODES; x++) {
		s->node_up[x] = true;
		s->link_up[x][BUS_A] = s->link_up[x][BUS_B] = true;
	}
	s->cockpit_ok = true;
}

/* addLink / dropLink / addNode / dropNode (Part 4) */
static void net_apply(struct net_state *s, uint8_t e)
{
	int x = ev_node(e), y = ev_bus(e);

	switch (ev_kind(e)) {
	case EVK_COCKPIT:
		if (e == EV_COCKPIT_OK) {
			s->cockpit_ok = true;
		} else if (e == EV_COCKPIT_ERROR) {
			s->cockpit_ok = false;
		}
		break;
	case EVK_NODE_UP: /* addNode: alive again, no links until addLink */
		s->node_up[x] = true;
		break;
	case EVK_NODE_DOWN: /* dropNode: the node and every link attached to it */
		s->node_up[x] = false;
		s->link_up[x][BUS_A] = s->link_up[x][BUS_B] = false;
		break;
	case EVK_LINK_UP:
		s->link_up[x][y] = true;
		break;
	case EVK_LINK_DOWN:
		s->link_up[x][y] = false;
		break;
	default:
		break; /* application events don't change the graph */
	}
}

/* ---------------------------------------------------------------- log */

uint32_t raft_entry_term(const struct raft *r, uint16_t i)
{
	if (i == 0 || i > r->last) {
		return 0;
	}
	for (int k = r->nruns - 1; k >= 0; k--) {
		if (r->runs[k].start <= i) {
			return r->runs[k].term;
		}
	}
	return 0;
}

static void persist(struct raft *r, uint16_t from)
{
	if (r->io.persist) {
		r->io.persist(r->io.ctx, r, from);
	}
}

static void set_replay(struct raft *r, uint16_t i, bool v)
{
#if RAFT_TRACK_REPLAY
	if (v) {
		r->replay[i / 8] |= (uint8_t)(1u << (i % 8));
	} else {
		r->replay[i / 8] &= (uint8_t)~(1u << (i % 8));
	}
#else
	(void)r;
	(void)i;
	(void)v;
#endif
}

static bool get_replay(const struct raft *r, uint16_t i)
{
#if RAFT_TRACK_REPLAY
	return r->replay[i / 8] & (1u << (i % 8));
#else
	(void)r;
	(void)i;
	return false;
#endif
}

static bool log_push(struct raft *r, uint32_t term, uint8_t e)
{
	if (r->last >= RAFT_CONF_LOG_SIZE) {
		return false;
	}
	if (r->nruns == 0 || r->runs[r->nruns - 1].term != term) {
		if (r->nruns == RAFT_TERM_RUNS) {
			return false;
		}
		r->runs[r->nruns].start = (uint16_t)(r->last + 1);
		r->runs[r->nruns].term = term;
		r->nruns++;
	}
	r->ev[++r->last] = e;
	set_replay(r, r->last, false);
	net_apply(&r->tail, e);
	return true;
}

static bool log_append(struct raft *r, uint32_t term, uint8_t e)
{
	if (!log_push(r, term, e)) {
		return false;
	}
	persist(r, r->last);
	return true;
}

/* drop entries from index `from` on (never committed ones: Raft guarantees it) */
static void log_truncate(struct raft *r, uint16_t from)
{
	if (from > r->last) {
		return;
	}
	r->last = (uint16_t)(from - 1);
	while (r->nruns && r->runs[r->nruns - 1].start > r->last) {
		r->nruns--;
	}
	net_init(&r->tail);
	for (uint16_t i = 1; i <= r->last; i++) {
		net_apply(&r->tail, r->ev[i]);
	}
	persist(r, from);
}

static void log_clear(struct raft *r)
{
	r->last = 0;
	r->nruns = 0;
	r->commit = r->applied_idx = 0;
	net_init(&r->net);
	net_init(&r->tail);
	persist(r, 1);
}

static void apply_committed(struct raft *r)
{
	while (r->applied_idx < r->commit) {
		uint16_t i = ++r->applied_idx;

		net_apply(&r->net, r->ev[i]);
		if (r->io.applied) {
			r->io.applied(r->io.ctx, i, raft_entry_term(r, i), r->ev[i], get_replay(r, i));
		}
	}
}

/* ---------------------------------------------------------------- send helpers */

static void send(struct raft *r, uint16_t base, uint8_t d[MSG_DLC])
{
	r->io.send(r->io.ctx, RAFT_ID(base, r->me), d);
}

static void send_append(struct raft *r, uint16_t index)
{
	struct raft_append m = {
		.term = r->term,
		.index = index,
		.prev_term = raft_entry_term(r, (uint16_t)(index - 1)),
		.entry_term = index <= r->last ? raft_entry_term(r, index) : 0,
		.event = index <= r->last ? r->ev[index] : EV_NONE,
		.commit = r->commit,
	};
	uint8_t d[MSG_DLC];

	append_encode(&m, d);
	send(r, ID_APPEND_BASE, d);
	r->last_append_ms = r->now;
}

static void send_ack(struct raft *r, bool ok, uint16_t match)
{
	struct raft_ack m = { .term = r->term, .success = ok, .match = match };
	uint8_t d[MSG_DLC];

	ack_encode(&m, d);
	send(r, ID_APPEND_ACK_BASE, d);
}

static void send_client_rsp(struct raft *r, uint8_t client, uint8_t req, uint8_t status,
			    uint16_t index)
{
	struct raft_client_rsp m = { .term = r->term, .client = client, .req_id = req,
				     .status = status, .leader = r->leader, .index = index };
	uint8_t d[MSG_DLC];

	client_rsp_encode(&m, d);
	send(r, ID_CLIENT_RSP_BASE, d);
}

/* ---------------------------------------------------------------- roles */

static void reset_election(struct raft *r)
{
	r->leader_seen_ms = r->now;
	r->election_ms = RAFT_ELECTION_MIN_MS + raft_rand_next(&r->rng) % (RAFT_CONF_JITTER_MS + 1);
}

static void set_term(struct raft *r, uint32_t t)
{
	if (t > r->term) {
		r->term = t;
		r->voted_for = ZONE_NONE;
		persist(r, 0);
	}
}

static void become_follower(struct raft *r, uint8_t leader)
{
	if (r->role != ROLE_FOLLOWER || r->leader != leader) {
		LOG(r, "raft: term %lu follower, leader %s", (unsigned long)r->term,
		    node_name[leader]);
	}
	r->role = ROLE_FOLLOWER;
	r->leader = leader;
	memset(r->pend, 0, sizeof(r->pend));
}

static void become_leader(struct raft *r, bool log_noop)
{
	r->role = ROLE_LEADER;
	r->leader = r->me;
	for (int x = 1; x <= RAFT_NODES; x++) {
		r->f[x] = (struct follower){ .next = (uint16_t)(r->last + 1) };
	}
	memset(r->pend, 0, sizeof(r->pend));
	memset(r->last_req_valid, 0, sizeof(r->last_req_valid));
	LOG(r, "raft: term %lu LEADER %s", (unsigned long)r->term, node_name[r->me]);
	if (log_noop) {
		log_append(r, r->term, EV_LEADER(r->me));
	}
	r->commit_dirty = true; /* announce ourselves with an APPEND right away */
}

static void start_election(struct raft *r)
{
	r->term++;
	r->voted_for = r->me;
	r->votes = BIT(r->me);
	r->role = ROLE_CANDIDATE;
	r->leader = ZONE_NONE;
	persist(r, 0);
	reset_election(r);
	LOG(r, "raft: term %lu election, %s is candidate", (unsigned long)r->term,
	    node_name[r->me]);

	struct raft_vote_req m = { .term = r->term, .last_index = r->last,
				   .last_term = raft_entry_term(r, r->last) };
	uint8_t d[MSG_DLC];

	vote_req_encode(&m, d);
	send(r, ID_VOTE_REQ_BASE, d);
}

bool raft_isolated(const struct raft *r)
{
	for (int x = 1; x <= RAFT_NODES; x++) {
		if (x != r->me && r->heard_any[x] && since(r, r->heard_ms[x]) < RAFT_PEER_WINDOW_MS) {
			return false;
		}
	}
	return true;
}

static bool can_campaign(const struct raft *r)
{
	if (r->role == ROLE_LEADER || r->role == RAFT_BOOT || raft_isolated(r)) {
		return false;
	}
	/* with a pre-defined leader, nobody campaigns until that leader has been heard once,
	 * so the STM32s don't elect themselves while the Pi is still booting */
	if (RAFT_CONF_DEFAULT_LEADER != RAFT_LEADER_ELECT) {
		return r->heard_leader_once;
	}
	return r->heard_leader_once || r->formed;
}

/* a valid REF/APPEND from `sender` in term t: follow it */
static void leader_contact(struct raft *r, uint8_t sender, uint32_t t)
{
	if (r->role == RAFT_BOOT) {
		LOG(r, "raft: boot, joining running cluster (term %lu, leader %s)",
		    (unsigned long)t, node_name[sender]);
		r->role = ROLE_FOLLOWER;
	}
	set_term(r, t);
	if (r->role != ROLE_FOLLOWER || r->leader != sender) {
		become_follower(r, sender);
	}
	r->heard_leader_once = true;
	reset_election(r);
}

/* ---------------------------------------------------------------- init / restore */

void raft_init(struct raft *r, uint8_t me, const struct raft_io *io, uint32_t now)
{
	memset(r, 0, sizeof(*r));
	r->me = me;
	r->io = *io;
	r->now = r->boot_ms = r->leader_seen_ms = now;
	r->rng = raft_rand_init(RAFT_CONF_SEED, me);
	r->role = RAFT_BOOT;
	r->leader = ZONE_NONE;
	net_init(&r->net);
	net_init(&r->tail);
	r->req_id = (uint8_t)(now * 7 + me); /* differs across reboots: a retry is never mistaken */
}

void raft_restore(struct raft *r, uint32_t term, uint8_t voted_for)
{
	r->term = term;
	r->voted_for = voted_for;
}

bool raft_restore_entry(struct raft *r, uint16_t index, uint32_t term, uint8_t e)
{
	if (index != r->last + 1 || !ev_valid(e) || term < raft_entry_term(r, r->last)) {
		return false;
	}
	return log_push(r, term, e);
}

/* boot: listen for a running cluster before taking the pre-defined leadership */
static void boot_decide(struct raft *r)
{
	bool listened = since(r, r->boot_ms) >= RAFT_BOOT_LISTEN_MS;

	if (RAFT_CONF_DEFAULT_LEADER != RAFT_LEADER_ELECT) {
		if (!listened) {
			return;
		}
		/* nobody leading: this is a cold start of the whole vehicle */
		LOG(r, "raft: cold start, default leader %s",
		    node_name[RAFT_CONF_DEFAULT_LEADER]);
		log_clear(r);
		r->term = 1;
		r->voted_for = ZONE_NONE;
		persist(r, 0);
		reset_election(r);
		if (r->me == RAFT_CONF_DEFAULT_LEADER) {
			become_leader(r, false);
			r->heard_leader_once = true;
		} else {
			r->role = ROLE_FOLLOWER;
			r->leader = RAFT_CONF_DEFAULT_LEADER;
		}
		return;
	}
	/* election modes: wait the same listen time after the cluster forms on every node, so
	 * the STM32s (up long before the Pi) and the Pi start their election timers together */
	if (!listened || !r->formed || since(r, r->formed_ms) < RAFT_BOOT_LISTEN_MS) {
		return;
	}
	LOG(r, "raft: cold start, all nodes heard, electing");
	log_clear(r);
	r->term = 0;
	r->voted_for = ZONE_NONE;
	persist(r, 0);
	r->role = ROLE_FOLLOWER;
	r->leader = ZONE_NONE;
	reset_election(r); /* first draw of the seeded PRNG: see tools/find_seed.py */
}

/* ---------------------------------------------------------------- receive */

void raft_heard(struct raft *r, uint8_t zone, int bus, bool relayed, uint32_t now)
{
	if (zone == r->me || zone < ZONE_N1 || zone > ZONE_N3) {
		return;
	}
	r->now = now;
	r->heard_ms[zone] = now;
	r->heard_any[zone] = true;
	if (!relayed) {
		r->heard_bus_ms[zone][bus] = now;
	}
	if (!r->formed) {
		bool all = true;

		for (int x = 1; x <= RAFT_NODES; x++) {
			all &= x == r->me || r->heard_any[x];
		}
		if (all) {
			r->formed = true;
			r->formed_ms = now;
		}
	}
}

bool raft_ref_is_master(const struct raft *r, uint8_t sender, const uint8_t d[MSG_DLC])
{
	return r->role == ROLE_FOLLOWER && r->leader == sender && d[1] == (uint8_t)r->term;
}

bool raft_ref_info(const struct raft *r, uint32_t *term, uint8_t *mode)
{
	if (r->role != ROLE_LEADER) {
		return false;
	}
	*term = r->term;
	*mode = r->net.cockpit_ok ? 0 : REF_MODE_FAILURE;
	return true;
}

void raft_on_ref(struct raft *r, uint8_t sender, const uint8_t d[MSG_DLC], uint32_t now)
{
	struct raft_ref m;

	r->now = now;
	if (!ref_decode(d, r->term, &m) || m.term < r->term || m.term == 0) {
		return; /* a deposed leader; its APPENDs will get a higher-term ACK */
	}
	if (r->role == ROLE_LEADER && m.term == r->term) {
		return; /* cannot happen in Raft; ignore rather than follow */
	}
	leader_contact(r, sender, m.term);
}

static void on_append(struct raft *r, uint8_t sender, const struct raft_append *m)
{
	if (m->term < r->term) {
		if (r->role != RAFT_BOOT) {
			send_ack(r, false, r->last); /* tells the stale leader our term */
		}
		return;
	}
	if (r->role == ROLE_LEADER && m->term == r->term) {
		return;
	}
	leader_contact(r, sender, m->term);

	uint16_t prev = (uint16_t)(m->index - 1);

	if (prev > r->last) {
		send_ack(r, false, r->last);
		return;
	}
	if (raft_entry_term(r, prev) != m->prev_term) {
		send_ack(r, false, prev > 0 ? (uint16_t)(prev - 1) : 0);
		return;
	}
	uint16_t match = prev;

	if (m->event != EV_NONE) {
		if (m->index <= r->last && raft_entry_term(r, m->index) != m->entry_term) {
			LOG(r, "raft: conflict at %u, truncating", m->index);
			log_truncate(r, m->index);
		}
		if (m->index == r->last + 1) {
			if (!log_append(r, m->entry_term, m->event)) {
				send_ack(r, false, r->last); /* full: the leader will retry */
				return;
			}
			/* already committed when the leader sent it: we are catching up */
			set_replay(r, m->index, m->commit >= m->index);
			if (RAFT_CONF_DEBUG) {
				char b[24];

				LOG(r, "raft: store %u term %lu %s", m->index,
				    (unsigned long)m->entry_term, raft_event_name(m->event, b, sizeof(b)));
			}
		}
		match = m->index;
	}
	uint16_t c = m->commit < match ? m->commit : match;

	if (c > r->commit) {
		r->commit = c;
		apply_committed(r);
	}
	send_ack(r, true, match);
}

static void leader_commit(struct raft *r);

static void on_ack(struct raft *r, uint8_t sender, const struct raft_ack *m)
{
	if (m->term > r->term) {
		set_term(r, m->term);
		become_follower(r, ZONE_NONE);
		reset_election(r);
		return;
	}
	if (r->role != ROLE_LEADER || m->term != r->term) {
		return;
	}
	struct follower *f = &r->f[sender];

	if (m->success) {
		if (m->match > f->match) {
			f->match = m->match;
		}
		if (f->next < f->match + 1) {
			f->next = (uint16_t)(f->match + 1);
		}
	} else if (m->match + 1 < f->next) {
		f->next = (uint16_t)(m->match + 1);
	}
	f->inflight = false;
	leader_commit(r);
}

static void on_vote_req(struct raft *r, uint8_t sender, const struct raft_vote_req *m)
{
	if (r->role == RAFT_BOOT) {
		return; /* a rebooted node may have voted in this term already: stay out of it */
	}
	/* disruption guard (Raft thesis 4.2.3): while we hear a live leader, ignore it */
	bool sticky = (r->role == ROLE_FOLLOWER && r->leader != ZONE_NONE &&
		       since(r, r->leader_seen_ms) < RAFT_ELECTION_MIN_MS) ||
		      (r->role == ROLE_LEADER && !raft_isolated(r));

	if (m->term > r->term && sticky) {
		return;
	}
	if (m->term > r->term) {
		set_term(r, m->term);
		become_follower(r, ZONE_NONE);
	}
	uint32_t my_last_term = raft_entry_term(r, r->last);
	bool up_to_date = m->last_term > my_last_term ||
			  (m->last_term == my_last_term && m->last_index >= r->last);
	bool grant = m->term == r->term && up_to_date &&
		     (r->voted_for == ZONE_NONE || r->voted_for == sender);

	if (grant) {
		r->voted_for = sender;
		persist(r, 0);
		reset_election(r);
		LOG(r, "raft: term %lu vote for %s", (unsigned long)r->term, node_name[sender]);
	}
	struct raft_vote_rsp v = { .term = r->term, .candidate = sender, .granted = grant };
	uint8_t d[MSG_DLC];

	vote_rsp_encode(&v, d);
	send(r, ID_VOTE_RSP_BASE, d);
}

static void on_vote_rsp(struct raft *r, uint8_t sender, const struct raft_vote_rsp *m)
{
	if (m->term > r->term) {
		set_term(r, m->term);
		become_follower(r, ZONE_NONE);
		reset_election(r);
		return;
	}
	if (r->role != ROLE_CANDIDATE || m->term != r->term || m->candidate != r->me ||
	    !m->granted) {
		return;
	}
	r->votes |= BIT(sender);
	int n = 0;

	for (int x = 1; x <= RAFT_NODES; x++) {
		n += !!(r->votes & BIT(x));
	}
	if (n >= RAFT_MAJORITY) {
		become_leader(r, true);
	}
}

/* ---------------------------------------------------------------- leader: client requests */

static bool view_node_up(const struct raft *r, int x)
{
	return x == r->me || (r->heard_any[x] && since(r, r->heard_ms[x]) < RAFT_NODE_TIMEOUT_MS);
}

/* append the event a client asked for; returns CL_OK/CL_BUSY and the index (0: nothing) */
static uint8_t leader_submit(struct raft *r, uint8_t e, uint16_t *index)
{
	int x = ev_node(e);

	*index = 0;
	switch (ev_kind(e)) {
	case EVK_NODE_UP:
	case EVK_NODE_DOWN: {
		bool up = ev_kind(e) == EVK_NODE_UP;

		/* node liveness is the leader's call: drop reports it cannot confirm */
		if (r->tail.node_up[x] == up || view_node_up(r, x) != up) {
			return CL_OK;
		}
		break;
	}
	case EVK_LINK_UP:
	case EVK_LINK_DOWN:
		if (r->tail.link_up[x][ev_bus(e)] == (ev_kind(e) == EVK_LINK_UP)) {
			return CL_OK; /* already so: one MEMBER entry per change */
		}
		break;
	case EVK_COCKPIT:
		return CL_OK; /* the leader logs these itself, with NODE_1 changes */
	default:
		break;
	}
	bool cockpit = x == ZONE_N1 && (ev_kind(e) == EVK_NODE_UP || ev_kind(e) == EVK_NODE_DOWN);

	if (r->last + 2 > RAFT_CONF_LOG_SIZE || !log_append(r, r->term, e)) {
		return CL_BUSY;
	}
	if (cockpit) {
		log_append(r, r->term, ev_kind(e) == EVK_NODE_UP ? EV_COCKPIT_OK : EV_COCKPIT_ERROR);
	}
	*index = r->last;
	if (RAFT_CONF_DEBUG) {
		char b[24];

		LOG(r, "raft: append %u term %lu %s", r->last, (unsigned long)r->term,
		    raft_event_name(e, b, sizeof(b)));
	}
	return CL_OK;
}

static void on_client_req(struct raft *r, uint8_t sender, const struct raft_client_req *m)
{
	if (m->dst != r->me) {
		return;
	}
	if (r->role != ROLE_LEADER) {
		send_client_rsp(r, sender, m->req_id, CL_REDIRECT, 0);
		return;
	}
	/* a retry of the request we already have */
	if (r->last_req_valid[sender] && r->last_req[sender] == m->req_id) {
		uint16_t i = r->last_req_idx[sender];

		if (i == 0 || i <= r->commit) {
			send_client_rsp(r, sender, m->req_id, CL_OK, i);
		}
		return; /* still pending: the OK goes out at commit */
	}
	uint16_t index;
	uint8_t st = leader_submit(r, m->event, &index);

	if (st != CL_OK) {
		send_client_rsp(r, sender, m->req_id, st, 0);
		return;
	}
	r->last_req_valid[sender] = true;
	r->last_req[sender] = m->req_id;
	r->last_req_idx[sender] = index;
	if (index == 0) {
		send_client_rsp(r, sender, m->req_id, CL_OK, 0);
		return;
	}
	for (int k = 0; k < RAFT_PENDING; k++) {
		if (!r->pend[k].used) {
			r->pend[k] = (struct pending_rsp){ sender, m->req_id, index, true };
			return;
		}
	}
	/* table full: the client retries and gets its OK from last_req once committed */
}

static void leader_commit(struct raft *r)
{
	uint16_t old = r->commit;

	for (uint16_t n = r->last; n > r->commit; n--) {
		if (raft_entry_term(r, n) != r->term) {
			break; /* only entries of our own term commit by counting (Raft 5.4.2) */
		}
		int count = 1;

		for (int x = 1; x <= RAFT_NODES; x++) {
			count += x != r->me && r->f[x].match >= n;
		}
		if (count >= RAFT_MAJORITY) {
			r->commit = n;
			break;
		}
	}
	if (r->commit == old) {
		return;
	}
	apply_committed(r);
	r->commit_dirty = true;
	for (int k = 0; k < RAFT_PENDING; k++) {
		if (r->pend[k].used && r->pend[k].index <= r->commit) {
			send_client_rsp(r, r->pend[k].client, r->pend[k].req_id, CL_OK, r->pend[k].index);
			r->pend[k].used = false;
		}
	}
}

/* ---------------------------------------------------------------- client side */

static void client_done(struct raft *r)
{
	r->cl_inflight = false;
	if (r->cl_is_app) {
		r->app_head = (uint8_t)((r->app_head + 1) % RAFT_APP_QUEUE);
		r->app_n--;
		return;
	}
	int x = ev_node(r->cl_event), y = ev_bus(r->cl_event);

	r->cl_hold_ms[x][y < 0 ? 0 : y + 1] = r->now;
}

static void on_client_rsp(struct raft *r, const struct raft_client_rsp *m)
{
	if (m->client != r->me || !r->cl_inflight || m->req_id != r->cl_req) {
		return;
	}
	if (m->status == CL_OK) {
		client_done(r);
	} else if (m->status == CL_REDIRECT) {
		r->cl_target = m->leader;
		r->cl_sent_ms = r->now - RAFT_CLIENT_RETRY_MS; /* resend on this tick */
	}
}

void raft_report(struct raft *r, uint8_t e)
{
	if (r->app_n == RAFT_APP_QUEUE || !ev_valid(e)) {
		return; /* full: drop the newest rather than block a caller on the CAN path */
	}
	r->app_q[(r->app_head + r->app_n) % RAFT_APP_QUEUE] = e;
	r->app_n++;
}

/* the first item where what this node sees differs from the committed graph */
static bool client_pick(struct raft *r, uint8_t *e)
{
	bool up = since(r, r->boot_ms) >= RAFT_NODE_TIMEOUT_MS; /* views settled after boot */

	for (int x = 1; up && x <= RAFT_NODES; x++) {
		bool v = view_node_up(r, x);

		if (v != r->net.node_up[x] && since(r, r->cl_hold_ms[x][0]) >= RAFT_CLIENT_HOLD_MS) {
			*e = v ? EV_NODE_UP(x) : EV_NODE_DOWN(x);
			return true;
		}
	}
	for (int y = 0; up && y < 2; y++) {
		bool v = false;

		for (int x = 1; x <= RAFT_NODES; x++) {
			v |= x != r->me && r->heard_any[x] &&
			     since(r, r->heard_bus_ms[x][y]) < RAFT_LINK_TIMEOUT_MS;
		}
		/* report our own links only, and only once the graph has us as a node */
		if (r->net.node_up[r->me] && v != r->net.link_up[r->me][y] &&
		    since(r, r->cl_hold_ms[r->me][y + 1]) >= RAFT_CLIENT_HOLD_MS) {
			*e = v ? EV_LINK_UP(r->me, y) : EV_LINK_DOWN(r->me, y);
			return true;
		}
	}
	return false;
}

static void client_send(struct raft *r)
{
	r->cl_sent_ms = r->now;
	if (r->role == ROLE_LEADER) {
		uint16_t index;

		if (leader_submit(r, r->cl_event, &index) == CL_OK) {
			client_done(r);
		}
		return;
	}
	/* a redirect hint wins until the next retry; otherwise whoever we follow */
	uint8_t t = (r->cl_target != ZONE_NONE && r->cl_target != r->me) ? r->cl_target : r->leader;

	if (t == ZONE_NONE || t == r->me) {
		return; /* no leader yet: hold it and retry */
	}
	r->cl_target = t;

	struct raft_client_req m = { .term = r->term, .dst = t, .req_id = r->cl_req,
				     .event = r->cl_event };
	uint8_t d[MSG_DLC];

	client_req_encode(&m, d);
	send(r, ID_CLIENT_REQ_BASE, d);
}

static void client_tick(struct raft *r)
{
	if (r->role == RAFT_BOOT) {
		return;
	}
	if (r->cl_inflight) {
		if (since(r, r->cl_sent_ms) < RAFT_CLIENT_RETRY_MS) {
			return;
		}
		uint8_t e;

		/* a graph report that no longer matches what we see is withdrawn */
		if (!r->cl_is_app && (!client_pick(r, &e) || e != r->cl_event)) {
			r->cl_inflight = false;
		} else {
			if (r->leader != ZONE_NONE && r->cl_target != r->leader) {
				r->cl_target = r->leader;
			}
			client_send(r);
			return;
		}
	}
	uint8_t e;

	if (client_pick(r, &e)) {
		r->cl_is_app = false;
	} else if (r->app_n) {
		e = r->app_q[r->app_head];
		r->cl_is_app = true;
	} else {
		return;
	}
	r->cl_event = e;
	r->cl_req = ++r->req_id;
	r->cl_inflight = true;
	r->cl_target = r->leader;
	client_send(r);
}

/* ---------------------------------------------------------------- frames, tick */

void raft_on_frame(struct raft *r, uint16_t id, const uint8_t d[MSG_DLC], uint32_t now)
{
	uint8_t sender = (uint8_t)msg_sender(id, d);

	r->now = now;
	if (sender == r->me || !zone_ok(sender)) {
		return;
	}
	switch (raft_base(id)) {
	case ID_APPEND_BASE: {
		struct raft_append m;

		if (append_decode(d, r->term, &m)) {
			on_append(r, sender, &m);
		}
		break;
	}
	case ID_APPEND_ACK_BASE: {
		struct raft_ack m;

		if (r->role != RAFT_BOOT && ack_decode(d, r->term, &m)) {
			on_ack(r, sender, &m);
		}
		break;
	}
	case ID_VOTE_REQ_BASE: {
		struct raft_vote_req m;

		if (vote_req_decode(d, r->term, &m)) {
			on_vote_req(r, sender, &m);
		}
		break;
	}
	case ID_VOTE_RSP_BASE: {
		struct raft_vote_rsp m;

		if (r->role != RAFT_BOOT && vote_rsp_decode(d, r->term, &m)) {
			on_vote_rsp(r, sender, &m);
		}
		break;
	}
	case ID_CLIENT_REQ_BASE: {
		struct raft_client_req m;

		if (r->role != RAFT_BOOT && client_req_decode(d, r->term, &m)) {
			on_client_req(r, sender, &m);
		}
		break;
	}
	case ID_CLIENT_RSP_BASE: {
		struct raft_client_rsp m;

		if (client_rsp_decode(d, r->term, &m)) {
			on_client_rsp(r, &m);
		}
		break;
	}
	default:
		break; /* REF goes to raft_on_ref */
	}
}

static void leader_tick(struct raft *r)
{
	/* hearing nobody: we cannot be leading anyone; stop without a new term */
	bool alone = true;

	for (int x = 1; x <= RAFT_NODES; x++) {
		alone &= x == r->me || !r->heard_any[x] ||
			 since(r, r->heard_ms[x]) >= RAFT_LEADER_ALONE_MS;
	}
	if (alone && since(r, r->boot_ms) >= RAFT_LEADER_ALONE_MS) {
		LOG(r, "raft: leader hears nobody, stepping down");
		become_follower(r, ZONE_NONE);
		reset_election(r);
		return;
	}

	/* one entry per RAFT_APPEND_GAP_MS, to the live follower furthest behind */
	if (since(r, r->last_entry_tx_ms) >= RAFT_APPEND_GAP_MS) {
		uint16_t idx = 0;

		for (int x = 1; x <= RAFT_NODES; x++) {
			struct follower *f = &r->f[x];

			if (x == r->me || f->next > r->last || !r->heard_any[x] ||
			    since(r, r->heard_ms[x]) >= RAFT_FOLLOWER_LIVE_MS ||
			    (f->inflight && since(r, f->sent_ms) < RAFT_APPEND_RETRY_MS)) {
				continue;
			}
			if (idx == 0 || f->next < idx) {
				idx = f->next;
			}
		}
		if (idx) {
			send_append(r, idx);
			r->last_entry_tx_ms = r->now;
			r->commit_dirty = false; /* the frame carries the commit index */
			for (int x = 1; x <= RAFT_NODES; x++) {
				if (x != r->me && r->f[x].next == idx) {
					r->f[x].inflight = true;
					r->f[x].sent_ms = r->now;
				}
			}
			return;
		}
	}
	if (r->commit_dirty || since(r, r->last_append_ms) >= RAFT_APPEND_IDLE_MS) {
		send_append(r, (uint16_t)(r->last + 1)); /* empty: prev check + commit */
		r->commit_dirty = false;
	}
}

void raft_tick(struct raft *r, uint32_t now)
{
	r->now = now;
	switch (r->role) {
	case RAFT_BOOT:
		boot_decide(r);
		break;
	case ROLE_LEADER:
		leader_tick(r);
		break;
	default:
		if (can_campaign(r) && since(r, r->leader_seen_ms) >= r->election_ms) {
			start_election(r);
		}
		break;
	}
	client_tick(r);
}

/* ---------------------------------------------------------------- printing */

const char *raft_event_name(uint8_t e, char *b, int n)
{
	static const char *const app[] = { "?", "TEST_BTN", "LEFT_ON", "RIGHT_ON", "HAZARD_ON",
					   "BLINK_OFF", "BAD_CTRL" };
	int x = ev_node(e);

	switch (ev_kind(e)) {
	case EVK_COCKPIT:
		if (x) {
			snprintf(b, n, "LEADER_%d", x);
		} else {
			snprintf(b, n, "%s", e == EV_COCKPIT_OK ? "COCKPIT_OK" :
					     e == EV_COCKPIT_ERROR ? "COCKPIT_ERROR" : "NONE");
		}
		break;
	case EVK_NODE_UP:
	case EVK_NODE_DOWN:
		snprintf(b, n, "NODE_%d_%s", x, ev_kind(e) == EVK_NODE_UP ? "UP" : "DOWN");
		break;
	case EVK_LINK_UP:
	case EVK_LINK_DOWN:
		snprintf(b, n, "CANLINK_%d_%c_%s", x, 'A' + ev_bus(e),
			 ev_kind(e) == EVK_LINK_UP ? "UP" : "DOWN");
		break;
	default: {
		int k = EV_APP_KIND(e);

		snprintf(b, n, "EVENT_%s_%d", k < (int)(sizeof(app) / sizeof(app[0])) ? app[k] : "?",
			 EV_APP_NODE(e));
		break;
	}
	}
	return b;
}

void net_state_print(const struct net_state *s, char *b, int n)
{
	int k = snprintf(b, n, "%s", s->cockpit_ok ? "cockpit ok" : "FAILURE MODE");

	for (int x = 1; x <= RAFT_NODES && k < n; x++) {
		k += snprintf(b + k, n - k, " | N%d %s A:%s B:%s", x, s->node_up[x] ? "up" : "DOWN",
			      s->link_up[x][BUS_A] ? "up" : "down", s->link_up[x][BUS_B] ? "up" : "down");
	}
}
