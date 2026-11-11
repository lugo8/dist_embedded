/*
 * Three Raft nodes on two simulated CAN buses, with the isolation jumpers, a relay,
 * power cycling and the Pi's log file. Runs the Lab 5 checkoff scenarios and checks them.
 *   make -C tests -f sim.mk   (builds once per default_leader mode and runs everything)
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "raft.h"
#include "route.h"

#define MAXQ 4096

static int verbose;
static int failures;

#define CHECK(cond, ...)                                              \
	do {                                                          \
		if (!(cond)) {                                        \
			printf("    FAIL line %d: ", __LINE__);       \
			printf(__VA_ARGS__);                          \
			printf("\n");                                 \
			failures++;                                   \
		}                                                     \
	} while (0)

struct applied_rec {
	uint16_t index;
	uint8_t event;
	bool replayed;
};

struct node {
	struct raft r;
	bool on;
	bool link[2];      /* jumper in */
	bool hb;           /* sending heartbeats (Pi: proxy running, STM32: not test-failed) */
	uint8_t ref_cycle;
	uint32_t hb_direct_ms[4][2]; /* last heartbeat heard directly from x on bus b */
	uint32_t hb_any_ms[4];       /* last heartbeat from x, any copy */
	uint32_t hb_gap_max[4];      /* longest heartbeat silence from x (R4.5: 65 ms = failed) */
	uint32_t any_ms[2];          /* last frame from anyone on bus b (for DEG) */
	uint32_t direct_ms[4][2];    /* last direct frame from x on bus b (forward-on-detect) */
	uint32_t seen[64];           /* recent message uids (dedup) */
	int nseen;
	int relayed_tx;
	int ref_from_master;
	struct applied_rec applied[2048];
	int napplied;
	/* the Pi's log file */
	uint32_t p_term;
	uint8_t p_vote;
	uint16_t p_last;
	uint8_t p_ev[RAFT_CONF_LOG_SIZE + 1];
	uint32_t p_tm[RAFT_CONF_LOG_SIZE + 1];
};

static struct node nodes[4];
static uint32_t now;
static int vote_reqs, appends_empty, appends_entry;

struct msg {
	uint32_t at;
	uint32_t uid;
	uint8_t from, to;
	uint16_t id;
	uint8_t d[8];
	int bus;
	bool relayed;
};
static uint32_t next_uid = 1;
static struct msg q[MAXQ];
static int qn;

static bool on_bus(int n, int b)
{
	return nodes[n].on && nodes[n].link[b];
}

/* node `tx` puts one copy of `from`'s frame on bus b: every other node on b receives it
 * (on real CAN the sender comes from the ID, so a relayed copy still names `from`) */
static void put_on_bus(uint8_t tx, uint8_t from, uint32_t uid, uint16_t id, const uint8_t d[8],
		       int b, bool relayed)
{
	if (!on_bus(tx, b)) {
		return; /* jumper out: the copy goes nowhere */
	}
	for (int to = 1; to <= 3; to++) {
		if (to != tx && to != from && on_bus(to, b)) {
			assert(qn < MAXQ);
			q[qn] = (struct msg){ now + 1, uid, from, (uint8_t)to, id, { 0 }, b, relayed };
			memcpy(q[qn].d, d, 8);
			qn++;
		}
	}
}

/* send_both: the same frame (same uid = same seq) on both buses */
static void transmit(uint8_t from, uint16_t id, const uint8_t d[8])
{
	if (!nodes[from].on) {
		return;
	}
	uint32_t uid = next_uid++;

	put_on_bus(from, from, uid, id, d, 0, false);
	put_on_bus(from, from, uid, id, d, 1, false);
}

static uint8_t my_deg(int n)
{
	uint8_t deg = 0;

	for (int b = 0; b < 2; b++) {
		if (now - nodes[n].any_ms[b] >= 45) {
			deg |= b ? HEALTH_B_DEG : HEALTH_A_DEG;
		}
	}
	return deg;
}

/* the relay decision as can_io.c / cockpit.c make it */
static void maybe_relay(int r, const struct msg *m)
{
	struct node *nd = &nodes[r];
	int to = 1 - m->bus;
	uint8_t deg = my_deg(r);

	bool hb_both[4] = { false };

	for (int x = 1; x <= 3; x++) {
		hb_both[x] = x != r && now - nd->hb_direct_ms[x][0] < ROUTE_HB_MS &&
			     now - nd->hb_direct_ms[x][1] < ROUTE_HB_MS && nd->hb_direct_ms[x][0] &&
			     nd->hb_direct_ms[x][1];
	}
	if (m->relayed || route_relay(&nd->r.net, hb_both, (uint8_t)r, nd->hb) != r ||
	    (deg & (to ? HEALTH_B_DEG : HEALTH_A_DEG)) || now - nd->direct_ms[m->from][to] < 30) {
		return;
	}
	nd->relayed_tx++;
	put_on_bus((uint8_t)r, m->from, m->uid, m->id, m->d, to, true);
}

static void io_send(void *ctx, uint16_t id, uint8_t d[8])
{
	int n = (int)(intptr_t)ctx;

	if (raft_base(id) == ID_VOTE_REQ_BASE) {
		vote_reqs++;
	}
	if (raft_base(id) == ID_APPEND_BASE) {
		if (d[6]) {
			appends_entry++;
		} else {
			appends_empty++;
		}
	}
	transmit((uint8_t)n, id, d);
}

static void io_applied(void *ctx, uint16_t index, uint32_t term, uint8_t e, bool replayed)
{
	struct node *nd = &nodes[(intptr_t)ctx];
	char b[32];

	(void)term;
	if (nd->napplied < 2048) {
		nd->applied[nd->napplied++] = (struct applied_rec){ index, e, replayed };
	}
	if (verbose) {
		printf("  [%6u] N%d applied %u %s%s\n", now, (int)(intptr_t)ctx, index,
		       raft_event_name(e, b, sizeof(b)), replayed ? " (replayed)" : "");
	}
}

static void io_persist(void *ctx, const struct raft *r, uint16_t from)
{
	struct node *nd = &nodes[(intptr_t)ctx];

	(void)from;
	nd->p_term = r->term;
	nd->p_vote = r->voted_for;
	nd->p_last = r->last;
	for (uint16_t i = 1; i <= r->last; i++) {
		nd->p_ev[i] = r->ev[i];
		nd->p_tm[i] = raft_entry_term(r, i);
	}
}

static void io_log(void *ctx, const char *fmt, ...)
{
	if (!verbose) {
		return;
	}
	va_list ap;

	printf("  [%6u] N%d ", now, (int)(intptr_t)ctx);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void power_on(int n, bool restore)
{
	struct node *nd = &nodes[n];
	struct raft_io io = { (void *)(intptr_t)n, io_send, io_applied,
			      n == ZONE_N1 ? io_persist : NULL, io_log };

	raft_init(&nd->r, (uint8_t)n, &io, now);
	if (restore) {
		raft_restore(&nd->r, nd->p_term, nd->p_vote);
		for (uint16_t i = 1; i <= nd->p_last; i++) {
			raft_restore_entry(&nd->r, i, nd->p_tm[i], nd->p_ev[i]);
		}
	}
	nd->on = true;
	nd->napplied = 0;
	nd->ref_cycle = 0;
}

static void reset_world(void)
{
	memset(nodes, 0, sizeof(nodes));
	qn = 0;
	now = 1000;
	vote_reqs = appends_empty = appends_entry = 0;
	for (int n = 1; n <= 3; n++) {
		nodes[n].link[0] = nodes[n].link[1] = true;
		nodes[n].hb = true;
	}
}

static bool seen_before(struct node *nd, uint32_t uid)
{
	for (int i = 0; i < nd->nseen; i++) {
		if (nd->seen[i] == uid) {
			return true;
		}
	}
	if (nd->nseen == 64) {
		memmove(nd->seen, nd->seen + 1, 63 * sizeof(nd->seen[0]));
		nd->nseen--;
	}
	nd->seen[nd->nseen++] = uid;
	return false;
}

static void deliver(void)
{
	int k = 0;
	static struct msg due[MAXQ];
	int nd_due = 0;

	for (int i = 0; i < qn; i++) {
		if (q[i].at > now) {
			q[k++] = q[i];
		} else {
			due[nd_due++] = q[i];
		}
	}
	qn = k;
	for (int i = 0; i < nd_due; i++) {
		struct msg *m = &due[i];
		struct node *nd = &nodes[m->to];

		if (!nd->on || !nd->link[m->bus]) {
			continue;
		}
		nd->any_ms[m->bus] = now;
		if (!m->relayed) {
			nd->direct_ms[m->from][m->bus] = now;
			if (m->id == ID_HB_N1) {
				nd->hb_direct_ms[m->from][m->bus] = now;
			}
		}
		raft_heard(&nd->r, m->from, m->bus, m->relayed, now);
		if (seen_before(nd, m->uid)) {
			continue;
		}
		maybe_relay(m->to, m);
		if (m->id == ID_HB_N1) {
			if (nd->hb_any_ms[m->from] && now - nd->hb_any_ms[m->from] > nd->hb_gap_max[m->from]) {
				nd->hb_gap_max[m->from] = now - nd->hb_any_ms[m->from];
			}
			nd->hb_any_ms[m->from] = now;
		} else if (raft_base(m->id) == ID_REF_BASE) {
			raft_on_ref(&nd->r, m->from, m->d, now);
			nd->ref_from_master += raft_ref_is_master(&nd->r, m->from, m->d);
		} else if (is_raft(m->id)) {
			raft_on_frame(&nd->r, m->id, m->d, now);
		}
	}
}

static void step(void)
{
	now++;
	deliver();
	for (int n = 1; n <= 3; n++) {
		struct node *nd = &nodes[n];

		if (!nd->on) {
			continue;
		}
		raft_tick(&nd->r, now);
		if (now % RAFT_CONF_HEARTBEAT_MS == (uint32_t)n) {
			uint8_t d[8] = { 0 };
			uint32_t term;
			uint8_t mode;

			if (nd->hb) {
				d[2] = my_deg(n);
				transmit((uint8_t)n, ID_HB_N1, d); /* stands in for HB_Nx: liveness + DEG */
				d[2] = 0;
			}
			if (raft_ref_info(&nd->r, &term, &mode)) {
				struct raft_ref m = { term, nd->ref_cycle++, now * 1000, mode, false };

				ref_encode(&m, d);
				transmit((uint8_t)n, ID_REF(n), d);
			}
		}
	}
}

static void run(uint32_t ms)
{
	for (uint32_t i = 0; i < ms; i++) {
		step();
	}
}

static int leaders(int *who)
{
	int c = 0;

	for (int n = 1; n <= 3; n++) {
		if (nodes[n].on && nodes[n].r.role == ROLE_LEADER) {
			c++;
			*who = n;
		}
	}
	return c;
}

/* run until exactly one leader, or timeout; returns ms taken or -1 */
static int await_leader(uint32_t limit, int *who)
{
	for (uint32_t t = 0; t < limit; t++) {
		if (leaders(who) == 1) {
			return (int)t;
		}
		step();
	}
	return -1;
}

/* committed logs of all running nodes agree (common prefix up to the lowest commit) */
static void check_logs_agree(const char *what)
{
	int ref = 0;

	for (int n = 1; n <= 3; n++) {
		if (nodes[n].on && (!ref || nodes[n].r.commit > nodes[ref].r.commit)) {
			ref = n;
		}
	}
	for (int n = 1; n <= 3; n++) {
		if (!nodes[n].on || n == ref) {
			continue;
		}
		struct raft *a = &nodes[n].r, *b = &nodes[ref].r;

		CHECK(a->commit == b->commit, "%s: N%d commit %u vs N%d %u", what, n, a->commit, ref,
		      b->commit);
		for (uint16_t i = 1; i <= a->commit && i <= b->commit; i++) {
			if (a->ev[i] != b->ev[i] || raft_entry_term(a, i) != raft_entry_term(b, i)) {
				CHECK(0, "%s: N%d and N%d differ at %u", what, n, ref, i);
				break;
			}
		}
		CHECK(memcmp(&a->net, &b->net, sizeof(a->net)) == 0, "%s: getNetwork differs N%d/N%d",
		      what, n, ref);
	}
}

static void reset_hb_gaps(void)
{
	for (int n = 1; n <= 3; n++) {
		memset(nodes[n].hb_gap_max, 0, sizeof(nodes[n].hb_gap_max));
	}
}

/* every live node kept hearing every other live, heartbeating node: no zone went to error */
static void check_hb_gaps(const char *what)
{
	for (int n = 1; n <= 3; n++) {
		for (int x = 1; x <= 3; x++) {
			if (x != n && nodes[n].on && nodes[x].on && nodes[x].hb) {
				CHECK(nodes[n].hb_gap_max[x] < 65, "%s: N%d missed N%d's heartbeat for %u ms",
				      what, n, x, nodes[n].hb_gap_max[x]);
			}
		}
	}
}

static int count_committed(int n, uint8_t e)
{
	int c = 0;

	for (uint16_t i = 1; i <= nodes[n].r.commit; i++) {
		c += nodes[n].r.ev[i] == e;
	}
	return c;
}

static void print_network(const char *label)
{
	char b[200];

	printf("    %s\n", label);
	for (int n = 1; n <= 3; n++) {
		if (!nodes[n].on) {
			printf("      N%d off\n", n);
			continue;
		}
		struct net_state s = raft_get_network(&nodes[n].r);

		net_state_print(&s, b, sizeof(b));
		printf("      N%d term %lu %s commit %u: %s\n", n, (unsigned long)nodes[n].r.term,
		       nodes[n].r.role == ROLE_LEADER ? "L" : "F", nodes[n].r.commit, b);
	}
}

/* checkoff step 1-2: STM32s up first, Pi (default leader) later */
static void cold_start_cockpit(void)
{
	reset_world();
	power_on(2, false);
	power_on(3, false);
	run(2000); /* Pi still booting */
	CHECK(leaders(&(int){ 0 }) == 0, "an STM32 led while waiting for the Pi");
	power_on(1, false);
	run(1000);
	int who = 0;

	CHECK(leaders(&who) == 1 && who == 1, "leader is N%d, want the Pi", who);
	for (int n = 1; n <= 3; n++) {
		CHECK(nodes[n].r.term == 1, "N%d term %lu, want 1", n, (unsigned long)nodes[n].r.term);
	}
	CHECK(vote_reqs == 0, "%d REQ_VOTE at boot", vote_reqs);
	int e0 = appends_empty;

	run(1000);
	CHECK(appends_empty - e0 >= 9 && appends_empty - e0 <= 11,
	      "%d empty APPEND per second, want 10", appends_empty - e0);
	check_logs_agree("cold start");
}

static void test_cold_start(void)
{
	printf("checkoff 1-2: cold start, LEADER=cockpit\n");
	cold_start_cockpit();
	print_network("network after cold start");
}

static void test_link_faults(void)
{
	printf("checkoff 4: link faults, no election\n");
	cold_start_cockpit();
	for (int n = 1; n <= 3; n++) {
		for (int b = 0; b < 2; b++) {
			uint8_t down = EV_LINK_DOWN(n, b), up = EV_LINK_UP(n, b);
			uint32_t t0 = nodes[1].r.term;

			reset_hb_gaps();
			nodes[n].link[b] = false;
			run(1000);
			check_hb_gaps("one link pulled");
			CHECK(count_committed(1, down) == 1, "N%d bus %c: %d DOWN entries", n, 'A' + b,
			      count_committed(1, down));
			CHECK(!nodes[1].r.net.link_up[n][b], "N%d bus %c not down in graph", n, 'A' + b);
			check_logs_agree("link down");
			if (n == 2 && b == 0) {
				print_network("network with N2 off bus A");
			}
			nodes[n].link[b] = true;
			run(1000);
			CHECK(count_committed(1, up) == 1, "N%d bus %c: %d UP entries", n, 'A' + b,
			      count_committed(1, up));
			for (int k = 1; k <= 3; k++) {
				CHECK(nodes[k].r.term == t0, "term changed on N%d", k);
			}
			check_logs_agree("link up");
		}
	}
	CHECK(vote_reqs == 0, "%d REQ_VOTE during link faults", vote_reqs);
}

static void test_two_links(void)
{
	printf("checkoff 5: N1 off B and N3 off A, event N2 -> N3 via the Pi's log\n");
	cold_start_cockpit();
	reset_hb_gaps();
	nodes[1].link[1] = false;
	nodes[3].link[0] = false;
	run(1500);
	check_hb_gaps("N1 off B, N3 off A");
	CHECK(count_committed(3, EV_LINK_DOWN(1, 1)) == 1, "N1_B down not in N3's log");
	CHECK(count_committed(3, EV_LINK_DOWN(3, 0)) == 1, "N3_A down not in N3's log");
	uint8_t ev = EV_APP_MAKE(APP_TEST_BTN, 2);

	raft_report(&nodes[2].r, ev);
	run(500);
	CHECK(count_committed(3, ev) == 1, "N2's event not in N3's log (%d)", count_committed(3, ev));
	for (int k = 1; k <= 3; k++) {
		CHECK(nodes[k].r.term == 1, "term changed on N%d", k);
	}
	print_network("network with N1 off B, N3 off A");
	check_logs_agree("two links");
}

static void test_pi_down_and_back(void)
{
	printf("checkoff 6-7: Pi off, STM32 leads, held events replayed when the Pi returns\n");
	cold_start_cockpit();
	run(500);
	nodes[1].on = false;
	int who = 0, t = await_leader(1000, &who);

	CHECK(t >= 0 && who != 1, "no STM32 leader within 1 s");
	printf("    new leader N%d after %d ms\n", who, t);
	run(1000 - (t < 0 ? 0 : t));
	CHECK(!nodes[2].r.net.cockpit_ok && !nodes[3].r.net.cockpit_ok, "not in failure mode");
	uint32_t ref_term;
	uint8_t mode = 0;

	CHECK(raft_ref_info(&nodes[who].r, &ref_term, &mode) && (mode & REF_MODE_FAILURE),
	      "REF mode bit not set");
	/* held events while the Pi is gone */
	uint16_t before = nodes[who].r.commit;

	raft_report(&nodes[2].r, EV_APP_MAKE(APP_LEFT_ON, 2));
	run(200);
	raft_report(&nodes[3].r, EV_APP_MAKE(APP_BAD_CTRL, 3));
	run(200);
	raft_report(&nodes[2].r, EV_APP_MAKE(APP_BLINK_OFF, 2));
	run(500);
	uint16_t held = (uint16_t)(nodes[who].r.commit - before);

	print_network("network with the Pi off");
	/* Pi back, restoring its log file */
	uint16_t pi_last = nodes[1].p_last;

	power_on(1, true);
	run(2000);
	CHECK(leaders(&who) == 1 && who != 1, "leadership left the STM32 (N%d)", who);
	CHECK(nodes[1].r.role == ROLE_FOLLOWER, "Pi is not a follower");
	CHECK(nodes[1].r.net.cockpit_ok, "still in failure mode after the Pi returned");
	/* every entry the Pi applied after its old log: each once, the held ones marked */
	int replayed = 0, dup = 0;

	for (int i = 0; i < nodes[1].napplied; i++) {
		struct applied_rec *a = &nodes[1].applied[i];

		replayed += a->replayed && a->index > pi_last;
		for (int j = 0; j < i; j++) {
			dup += nodes[1].applied[j].index == a->index;
		}
	}
	CHECK(dup == 0, "%d entries applied twice", dup);
	CHECK(replayed >= held, "%d replayed on the Pi, %u held", replayed, held);
	printf("    %u entries held while the Pi was off, %d replayed on its return\n", held, replayed);
	/* a live event after the catch-up is not marked replayed */
	uint8_t live = EV_APP_MAKE(APP_RIGHT_ON, 2);

	raft_report(&nodes[2].r, live);
	run(300);
	bool found = false;

	for (int i = 0; i < nodes[1].napplied; i++) {
		if (nodes[1].applied[i].event == live) {
			found = true;
			CHECK(!nodes[1].applied[i].replayed, "live event marked replayed");
		}
	}
	CHECK(found, "live event did not reach the Pi");
	print_network("network after the Pi rejoined");
	check_logs_agree("pi back");
}

static void test_relayed_sync(void)
{
	printf("checkoff 9: STM32 leading, the other STM32 cut off (scenario c): the Pi relays\n");
	cold_start_cockpit();
	run(500);
	nodes[1].on = false;
	int who = 0;

	await_leader(1000, &who);
	run(1000);
	power_on(1, true);
	run(2000);
	CHECK(leaders(&who) == 1 && who != 1, "no STM32 leader");
	int other = who == 2 ? 3 : 2;
	uint32_t term = nodes[who].r.term;

	/* leader off bus A, the other STM32 off bus B: they share no bus */
	run(200); /* let the Pi's boot-time heartbeats settle */
	reset_hb_gaps();
	nodes[who].link[0] = false;
	nodes[other].link[1] = false;
	int refs0 = nodes[other].ref_from_master, relays0 = nodes[1].relayed_tx;

	run(2000);
	check_hb_gaps("STM32s on different buses");
	int refs = nodes[other].ref_from_master - refs0;

	CHECK(refs >= 95, "cut-off N%d took %d REFs from its master in 2 s, want ~100", other, refs);
	CHECK(nodes[1].relayed_tx > relays0, "the Pi relayed nothing");
	for (int k = 1; k <= 3; k++) {
		CHECK(nodes[k].r.term == term, "term changed on N%d", k);
	}
	uint8_t ev = EV_APP_MAKE(APP_TEST_BTN, (uint8_t)other);

	raft_report(&nodes[other].r, ev);
	run(500);
	CHECK(count_committed(who, ev) == 1, "the cut-off node's event did not reach the leader");
	print_network("network with the STM32s on different buses");
	check_logs_agree("relayed sync");
	printf("    N%d leads, N%d cut off: %d REFs in 2 s via the Pi (%d frames relayed)\n", who,
	       other, refs, nodes[1].relayed_tx - relays0);
	nodes[who].link[0] = true;
	nodes[other].link[1] = true;
	run(1500);
	for (int k = 1; k <= 3; k++) {
		CHECK(nodes[k].r.term == term, "term changed on N%d after restore", k);
	}
	check_logs_agree("relayed sync restored");
}

static void test_proxy_stopped(void)
{
	printf("checkoff 1: proxy stopped, the Pi sends no heartbeat but still leads\n");
	reset_world();
	nodes[1].hb = false;
	power_on(2, false);
	power_on(3, false);
	run(1000);
	power_on(1, false);
	run(3000);
	int who = 0;

	CHECK(leaders(&who) == 1 && who == 1, "leader N%d, want the Pi", who);
	CHECK(nodes[2].r.net.cockpit_ok && nodes[2].r.net.node_up[1], "Pi treated as gone");
	CHECK(vote_reqs == 0, "%d REQ_VOTE", vote_reqs);
	nodes[1].hb = true; /* proxy started */
	run(500);
	check_logs_agree("proxy stopped");
}

static void test_isolation(void)
{
	printf("checkoff 10: isolating a follower keeps the term; isolating the leader bumps it once\n");
	cold_start_cockpit();
	nodes[3].link[0] = nodes[3].link[1] = false;
	run(2000);
	nodes[3].link[0] = nodes[3].link[1] = true;
	run(1500);
	for (int k = 1; k <= 3; k++) {
		CHECK(nodes[k].r.term == 1, "follower isolation: N%d term %lu", k,
		      (unsigned long)nodes[k].r.term);
	}
	check_logs_agree("follower isolation");

	nodes[1].link[0] = nodes[1].link[1] = false;
	run(2000);
	nodes[1].link[0] = nodes[1].link[1] = true;
	run(2000);
	int who = 0;

	for (int k = 1; k <= 3; k++) {
		CHECK(nodes[k].r.term == 2, "leader isolation: N%d term %lu, want 2", k,
		      (unsigned long)nodes[k].r.term);
	}
	CHECK(leaders(&who) == 1, "no single leader after the old leader returned");
	check_logs_agree("leader isolation");
}

static void test_random(void)
{
	printf("checkoff 8: LEADER=random (seed %u)\n", RAFT_CONF_SEED);
	reset_world();
	power_on(2, false);
	power_on(3, false);
	run(3000);
	CHECK(leaders(&(int){ 0 }) == 0, "STM32s elected before the Pi was up");
	power_on(1, false);
	int who = 0, t = await_leader(2000, &who);

	CHECK(t >= 0, "no leader");
	printf("    N%d won the boot election after %d ms (%d REQ_VOTE)\n", who, t, vote_reqs);
	run(1000);
	check_logs_agree("random boot");
	if (who != 1) {
		/* power off the STM32 leader: new leader within 1 s, no failure mode */
		nodes[who].on = false;
		int old = who;

		t = await_leader(1000, &who);
		CHECK(t >= 0 && who != old, "no new leader within 1 s");
		run(1000);
		CHECK(nodes[1].r.net.cockpit_ok, "failure mode after an STM32 went down");
		printf("    STM32 leader off: N%d leads after %d ms\n", who, t);
		check_logs_agree("random stm32 down");
	}
}

int main(int argc, char **argv)
{
	verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
	if (argc > 2) {
		if (!strcmp(argv[2], "two")) test_two_links();
		if (!strcmp(argv[2], "relay")) test_relayed_sync();
		return failures != 0;
	}
	if (RAFT_CONF_DEFAULT_LEADER == RAFT_LEADER_ELECT) {
		test_random();
	} else {
		test_cold_start();
		test_link_faults();
		test_two_links();
		test_pi_down_and_back();
		test_relayed_sync();
		test_proxy_stopped();
		test_isolation();
	}
	printf(failures ? "%d FAILURES\n" : "all scenarios passed\n", failures);
	return failures != 0;
}
