/*
 * Lab 5 on the Pi (N1). Same Raft agent/client/state machine and time sync code as the
 * STM32s (common/), run in threads inside cockpit:
 *   RAFT    2 ms tick; frames come in from cockpit's rx thread (raft_node_rx)
 *   REF_TX  every heartbeat_ms while leading, follow-up times from the driver's echo
 *   PPS     rising edge on GPIO17 at every whole second of global time, console line
 *   SAVE    writes the log file (term, vote, entries) when it changed, off the CAN path
 *   DRIFT   Part 6.2 printout once a second
 * The log file is what lets the Pi rejoin after a power cycle with the log it had: the
 * leader then only sends what it missed, and those entries come up marked replayed.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "raft.h"
#include "raft_node.h"
#include "route.h"
#include "timesync.h"

#define TICK_MS        2
#define STATUS_PRINT_MS 5000
#define SAVE_PERIOD_MS 100
#define PPS_EARLY_US   300
#define PPS_WIDTH_MS   100

static struct raft_node_hooks hk;
static const char *log_path;

static pthread_mutex_t raft_lock = PTHREAD_MUTEX_INITIALIZER;
static struct raft R;
static uint8_t seqs[MSG_COUNT];

static pthread_mutex_t ts_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timesync TS;

static int64_t last_direct_ref_ms;
static uint8_t ref_cycle;
static int64_t ref_done_us[2];
static int ref_done_cycle[2] = { -1, -1 };
static volatile int save_dirty;

/* ---------------------------------------------------------------- clocks */

int64_t pi_local_us(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC_RAW, &t); /* never slewed by NTP */
	return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

static uint32_t now_ms32(void)
{
	return (uint32_t)(pi_local_us() / 1000);
}

int64_t pi_global_gettime(void)
{
	pthread_mutex_lock(&ts_lock);
	int64_t g = ts_global(&TS, pi_local_us());

	pthread_mutex_unlock(&ts_lock);
	return g;
}

/*
 * Sleep until `us` on pi_local_us()'s clock. clock_nanosleep() refuses CLOCK_MONOTONIC_RAW
 * (it returns ENOTSUP at once), which left every thread here spinning at SCHED_FIFO, so
 * sleep relative on CLOCK_MONOTONIC for whatever is left and check again.
 */
static void sleep_until_us(int64_t us)
{
	for (;;) {
		int64_t d = us - pi_local_us();

		if (d <= 0) {
			return;
		}
		struct timespec t = { d / 1000000, (d % 1000000) * 1000 };

		nanosleep(&t, NULL); /* EINTR or an early wake: the loop works out what's left */
	}
}

/* ---------------------------------------------------------------- io */

static void io_send(void *ctx, uint16_t id, uint8_t d[MSG_DLC])
{
	(void)ctx;
	int k = msg_index(id);

	d[0] = (d[0] & 0xF0) | (seqs[k]++ & 0x0F);
	hk.send_both(id, d);
}

static void io_log(void *ctx, const char *fmt, ...)
{
	va_list ap;

	(void)ctx;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

static void io_applied(void *ctx, uint16_t index, uint32_t term, uint8_t e, bool replayed)
{
	char name[32];

	(void)ctx;
	printf("commit %u term %u %s%s\n", index, term, raft_event_name(e, name, sizeof(name)),
	       replayed ? "  (replayed)" : "");
	if (ev_kind(e) != EVK_APP && ev_kind(e) != EVK_COCKPIT) {
		char net[200];
		struct net_state s = raft_get_network(&R);

		net_state_print(&s, net, sizeof(net));
		printf("  getNetwork: %s\n", net);
	}
}

static void io_persist(void *ctx, const struct raft *r, uint16_t from)
{
	(void)ctx;
	(void)r;
	(void)from;
	save_dirty = 1; /* the SAVE thread writes it: an SD card write can take tens of ms */
}

/* ---------------------------------------------------------------- log file */

static void save_now(void)
{
	char tmp[256];

	snprintf(tmp, sizeof(tmp), "%s.tmp", log_path);
	FILE *f = fopen(tmp, "w");

	if (!f) {
		perror(tmp);
		return;
	}
	pthread_mutex_lock(&raft_lock);
	save_dirty = 0;
	fprintf(f, "term %u vote %u\n", R.term, R.voted_for);
	for (uint16_t i = 1; i <= R.last; i++) {
		fprintf(f, "%u %u %u\n", i, raft_entry_term(&R, i), R.ev[i]);
	}
	pthread_mutex_unlock(&raft_lock);
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	rename(tmp, log_path);
}

static void load(void)
{
	FILE *f = fopen(log_path, "r");
	unsigned term, vote, i, t, e, n = 0;

	if (!f) {
		printf("[raft] no log file %s: starting empty\n", log_path);
		return;
	}
	if (fscanf(f, "term %u vote %u\n", &term, &vote) == 2) {
		raft_restore(&R, term, (uint8_t)vote);
		while (fscanf(f, "%u %u %u\n", &i, &t, &e) == 3 &&
		       raft_restore_entry(&R, (uint16_t)i, t, (uint8_t)e)) {
			n++;
		}
	}
	fclose(f);
	printf("[raft] %s: term %u, %u entries (kept only if we join a running cluster)\n",
	       log_path, R.term, n);
}

static void *save_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { 0 };

	pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp); /* never ahead of CAN */
	for (;;) {
		usleep(SAVE_PERIOD_MS * 1000);
		if (save_dirty) {
			save_now();
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------- receive */

void raft_node_rx(int bus, uint16_t id, const uint8_t d[MSG_DLC], bool relayed, bool first,
		  int64_t rx_us)
{
	uint8_t zone = (uint8_t)msg_sender(id, d);
	uint32_t now = now_ms32();

	pthread_mutex_lock(&raft_lock);
	raft_heard(&R, zone, bus, relayed, now);
	if (is_hb(id)) {
		/* liveness only (raft_heard above); the relay choice uses hk.hb_both */
	} else if (raft_base(id) != ID_REF_BASE) {
		if (first && is_raft(id)) {
			raft_on_frame(&R, id, d, now);
		}
	} else {
		if (first) {
			raft_on_ref(&R, zone, d, now);
		}
		bool master = raft_ref_is_master(&R, zone, d);
		bool use = master && (!relayed || now - last_direct_ref_ms >= 3 * RAFT_CONF_HEARTBEAT_MS);

		if (master && !relayed) {
			last_direct_ref_ms = now;
		}
		if (use) {
			struct raft_ref m;

			ref_decode(d, R.term, &m);
			pthread_mutex_lock(&ts_lock);
			ts_on_ref(&TS, bus, m.cycle, m.followup, relayed, m.time_us, rx_us);
			pthread_mutex_unlock(&ts_lock);
		}
	}
	pthread_mutex_unlock(&raft_lock);
}

void raft_node_ref_echo(int bus, const uint8_t d[MSG_DLC], int64_t tx_us)
{
	pthread_mutex_lock(&ts_lock);
	ref_done_us[bus] = tx_us;
	ref_done_cycle[bus] = d[2];
	pthread_mutex_unlock(&ts_lock);
}

void raft_node_event(uint8_t kind)
{
	pthread_mutex_lock(&raft_lock);
	raft_report(&R, EV_APP_MAKE(kind, ZONE_N1));
	pthread_mutex_unlock(&raft_lock);
}

static volatile uint8_t sync_bits, relay_zone;

uint8_t raft_node_sync_bits(void)
{
	return sync_bits;
}

uint8_t raft_node_relay(void)
{
	return relay_zone;
}

/* ---------------------------------------------------------------- threads */

static void *agent_thread(void *arg)
{
	int64_t next = pi_local_us();

	uint8_t was = RAFT_BOOT; /* role at the last tick: a vote reply can make us leader in rx */

	(void)arg;
	for (;;) {
		next += TICK_MS * 1000;
		sleep_until_us(next);
		uint32_t now = now_ms32();

		pthread_mutex_lock(&raft_lock);
		raft_tick(&R, now);
		bool lead = R.role == ROLE_LEADER;

		pthread_mutex_lock(&ts_lock);
		if (lead && was != ROLE_LEADER) {
			ts_become_master(&TS, pi_local_us());
		}
		ts_check(&TS, pi_local_us());
		bool locked = TS.locked;
		bool drift = TS.syncs > 2 * TS_BASELINE_US / 1000 / RAFT_CONF_HEARTBEAT_MS;

		pthread_mutex_unlock(&ts_lock);
		bool hb_both[4] = { false, false, hk.hb_both(ZONE_N2), hk.hb_both(ZONE_N3) };

		relay_zone = route_relay(&R.net, hb_both, ZONE_N1, hk.sending_hb());
		was = R.role;
		pthread_mutex_unlock(&raft_lock);

		bool sync = lead || locked;

		sync_bits = (sync ? SYNC_LOCKED : 0) | (drift ? SYNC_DRIFT : 0);
		hk.gpio(PIN_LEADER, lead);
		hk.gpio(PIN_SYNC, sync);

		/* who leads, on every change (and the first time it is known) */
		static uint8_t shown_leader = 0xFF, shown_role = 0xFF;
		static uint32_t shown_term;

		pthread_mutex_lock(&raft_lock);
		uint8_t leader = R.leader, role = R.role;
		uint32_t term = R.term;
		pthread_mutex_unlock(&raft_lock);
		if (leader != shown_leader || term != shown_term || role != shown_role) {
			printf("leader: %s (term %u), this node %s\n", raft_node_name(leader), term,
			       raft_role_name(role));
			shown_leader = leader;
			shown_term = term;
			shown_role = role;
		}
	}
	return NULL;
}

static void *ref_thread(void *arg)
{
	int64_t next = pi_local_us();
	int pin = 0;

	(void)arg;
	for (;;) {
		next += RAFT_CONF_HEARTBEAT_MS * 1000;
		sleep_until_us(next);

		uint32_t term;
		uint8_t mode;

		pthread_mutex_lock(&raft_lock);
		bool lead = raft_ref_info(&R, &term, &mode);
		pthread_mutex_unlock(&raft_lock);
		if (!lead) {
			continue;
		}
		static uint8_t seq;
		uint8_t s = seq++ & 0x0F;

		for (int bus = 0; bus < 2; bus++) {
			pthread_mutex_lock(&ts_lock);
			bool fu = ref_done_cycle[bus] == (uint8_t)(ref_cycle - 1);
			int64_t t = ts_global(&TS, fu ? ref_done_us[bus] : pi_local_us());

			pthread_mutex_unlock(&ts_lock);
			struct raft_ref m = { .term = term, .cycle = ref_cycle, .time_us = (uint32_t)t,
					      .mode = mode, .followup = fu };
			uint8_t d[MSG_DLC];

			ref_encode(&m, d);
			d[0] |= s;
			hk.send_bus(bus, ID_REF(ZONE_N1), d);
		}
		ref_cycle++;
		hk.gpio(PIN_REF_TX, pin ^= 1);
	}
	return NULL;
}

static void *pps_thread(void *arg)
{
	(void)arg;
	for (;;) {
		int64_t g = pi_global_gettime();
		int64_t edge = (g / 1000000 + 1) * 1000000;

		sleep_until_us(pi_local_us() + (edge - g) - PPS_EARLY_US);
		int64_t l, limit = pi_local_us() + 2 * PPS_EARLY_US + 2000;

		while ((g = pi_global_gettime()) < edge && (l = pi_local_us()) < limit) {
		}
		if (g < edge || g > edge + 1000) {
			continue;
		}
		hk.gpio(PIN_PPS, 1);
		l = pi_local_us();
		pthread_mutex_lock(&ts_lock);
		struct timesync t = TS;

		pthread_mutex_unlock(&ts_lock);
		char ppm[12];

		/* Part 6.4: global and local time at the edge, drift ratio, last correction */
		printf("PPS %lld: global %lld.%06lld local %lld.%06lld drift %s ppm corr %+d us%s\n",
		       (long long)(edge / 1000000), (long long)(g / 1000000), (long long)(g % 1000000),
		       (long long)(l / 1000000), (long long)(l % 1000000), ts_ppm(ppm, t.ppb),
		       t.last_error_us,
		       t.locked ? "" : (R.role == ROLE_LEADER ? " (master)" : " UNSYNCED"));
		usleep(PPS_WIDTH_MS * 1000);
		hk.gpio(PIN_PPS, 0);
	}
	return NULL;
}

/* every 5 s: who leads, and (Part 6.2) the drift correction being applied */
static void *status_thread(void *arg)
{
	(void)arg;
	for (;;) {
		usleep(STATUS_PRINT_MS * 1000);
		pthread_mutex_lock(&raft_lock);
		uint8_t leader = R.leader, role = R.role;
		uint32_t term = R.term;
		pthread_mutex_unlock(&raft_lock);
		pthread_mutex_lock(&ts_lock);
		struct timesync t = TS;

		pthread_mutex_unlock(&ts_lock);
		char ppm[12];

		printf("status: leader %s, term %u, this node %s | time %s, drift %s ppm, "
		       "correcting %+d us\n",
		       raft_node_name(leader), term, raft_role_name(role),
		       role == ROLE_LEADER ? "master" : t.locked ? "locked" : "FREE",
		       ts_ppm(ppm, t.ppb), t.last_error_us);
	}
	return NULL;
}

/* ---------------------------------------------------------------- init */

void raft_node_init(const struct raft_node_hooks *h, const char *path)
{
	struct raft_io io = { .send = io_send, .applied = io_applied, .persist = io_persist,
			      .log = io_log };

	hk = *h;
	log_path = path;
	ts_init(&TS);
	raft_init(&R, ZONE_N1, &io, now_ms32());
	load();
	printf("[raft] cockpit, default leader %d, seed %u, log %d entries, file %s\n",
	       RAFT_CONF_DEFAULT_LEADER, RAFT_CONF_SEED, RAFT_CONF_LOG_SIZE, log_path);
}

void raft_node_start(void)
{
	pthread_t t;

	pthread_create(&t, NULL, agent_thread, NULL);
	pthread_create(&t, NULL, ref_thread, NULL);
	pthread_create(&t, NULL, pps_thread, NULL);
	pthread_create(&t, NULL, status_thread, NULL);
	pthread_create(&t, NULL, save_thread, NULL);
}
