/*
 * RAFT agent (Lab 5 Parts 1-6 on the STM32s): the Raft agent + client + state machine
 * from common/raft.c, the time sync from common/timesync.c, the relay choice from
 * common/route.h, REF_TX when leading, LEADER and SYNC LEDs, and the console printouts.
 *
 * Threads: RAFT (agent, 2 ms tick) owns struct raft; REF_TX sends REF every heartbeat_ms
 * while leading; PRINT does all console output. The dispatch thread only queues frames
 * here (raft_agent_rx), so nothing on the CTRL/brake path ever waits for Raft.
 */
#include <stdarg.h>
#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/spinlock.h>
#include <time.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/printk.h>
#include "route.h"
#include "timesync.h"
#include "zone.h"

#define USER DT_PATH(zephyr_user)

static const struct gpio_dt_spec leader_led = GPIO_DT_SPEC_GET(USER, leader_gpios);
static const struct gpio_dt_spec sync_led = GPIO_DT_SPEC_GET(USER, sync_gpios);

static struct raft R;
static K_MUTEX_DEFINE(raft_lock);

static struct timesync TS;
static struct k_spinlock ts_lock;

static atomic_t seqs[MSG_COUNT];
static atomic_t failure_mode;
static atomic_t sync_bits;

K_MSGQ_DEFINE(agent_q, sizeof(struct raft_rx), 32, 4);
K_MSGQ_DEFINE(event_q, sizeof(uint8_t), 16, 1);

/* ---------------------------------------------------------------- clocks */

int64_t local_us(void)
{
	return (int64_t)k_cyc_to_us_floor64(k_cycle_get_64());
}

int64_t global_gettime(void)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	int64_t g = ts_global(&TS, local_us());

	k_spin_unlock(&ts_lock, key);
	return g;
}

int64_t global_to_local(int64_t g)
{
	int64_t l = local_us();

	/* the model's rate is within 1000 ppm of 1: good to 1 ms over a second, and the
	 * callers finish by polling global_gettime() */
	return l + (g - global_gettime());
}

static void set_realtime(int64_t g)
{
	struct timespec t = { .tv_sec = g / 1000000, .tv_nsec = (long)(g % 1000000) * 1000 };

	sys_clock_settime(SYS_CLOCK_REALTIME, &t); /* what POSIX clock_settime() calls */
}

/* ---------------------------------------------------------------- printing */

enum { PR_APPLIED, PR_TEXT };
struct print_item {
	uint8_t kind;
	uint8_t event;
	bool replayed;
	uint16_t index;
	uint32_t term;
	struct net_state net; /* the network as of this commit, not as of printing */
	char text[120];
};
K_MSGQ_DEFINE(print_q, sizeof(struct print_item), 16, 4);

static void vqueue_text(const char *fmt, va_list ap)
{
	struct print_item p = { .kind = PR_TEXT };

	vsnprintk(p.text, sizeof(p.text), fmt, ap);
	k_msgq_put(&print_q, &p, K_NO_WAIT); /* full: drop it, never block the caller */
}

static void io_log(void *ctx, const char *fmt, ...)
{
	va_list ap;

	ARG_UNUSED(ctx);
	va_start(ap, fmt);
	vqueue_text(fmt, ap);
	va_end(ap);
}

void agent_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vqueue_text(fmt, ap);
	va_end(ap);
}

void raft_agent_ts(struct timesync *out)
{
	k_spinlock_key_t key = k_spin_lock(&ts_lock);

	*out = TS;
	k_spin_unlock(&ts_lock, key);
}

static void io_applied(void *ctx, uint16_t index, uint32_t term, uint8_t e, bool replayed)
{
	struct print_item p = { .kind = PR_APPLIED, .event = e, .replayed = replayed,
				.index = index, .term = term, .net = R.net };

	ARG_UNUSED(ctx); /* called by raft with raft_lock held, so R.net is this commit's state */
	k_msgq_put(&print_q, &p, K_NO_WAIT);
}

static void io_send(void *ctx, uint16_t id, uint8_t d[MSG_DLC])
{
	ARG_UNUSED(ctx);
	send_both(id, &seqs[msg_index(id)], d);
}

static void print_task(void *p1, void *p2, void *p3)
{
	struct print_item p;
	int64_t next_status = k_uptime_get() + STATUS_PRINT_MS;

	while (1) {
		int64_t wait = next_status - k_uptime_get();

		if (k_msgq_get(&print_q, &p, K_MSEC(wait > 0 ? wait : 0)) == 0) {
			if (p.kind == PR_TEXT) {
				printk("%s\n", p.text);
				continue;
			}
			char name[24], net[160];

			raft_event_name(p.event, name, sizeof(name));
			printk("[%lld] commit %u term %u %s%s\n", k_uptime_get(), p.index, p.term, name,
			       p.replayed ? " (replayed)" : "");
			if (ev_kind(p.event) != EVK_APP && ev_kind(p.event) != EVK_COCKPIT) {
				net_state_print(&p.net, net, sizeof(net));
				printk("  getNetwork: %s\n", net);
			}
			continue;
		}
		/* every 5 s: who leads, and (Part 6.2) the drift correction being applied */
		struct timesync t;
		k_spinlock_key_t key = k_spin_lock(&ts_lock);

		t = TS;
		k_spin_unlock(&ts_lock, key);
		k_mutex_lock(&raft_lock, K_FOREVER);
		uint8_t leader = R.leader, role = R.role;
		uint32_t term = R.term;
		k_mutex_unlock(&raft_lock);

		char ppm[12];

		printk("[%lld] status: leader %s, term %u, this node %s | time %s, drift %s ppm, "
		       "correcting %+d us\n",
		       k_uptime_get(), raft_node_name(leader), term, raft_role_name(role),
		       role == ROLE_LEADER ? "master" : t.locked ? "locked" : "FREE",
		       ts_ppm(ppm, t.ppb), t.last_error_us);
		next_status += STATUS_PRINT_MS;
	}
}

K_THREAD_DEFINE(print_tid, 2048, print_task, NULL, NULL, NULL, PRIO_PRINT, 0, SYS_FOREVER_MS);

/* ---------------------------------------------------------------- receive path */

void raft_agent_rx(const struct raft_rx *it)
{
	k_msgq_put(&agent_q, it, K_NO_WAIT);
}

void raft_agent_event(uint8_t kind)
{
	uint8_t e = EV_APP_MAKE(kind, MY_ZONE);

	k_msgq_put(&event_q, &e, K_NO_WAIT);
}

static int64_t last_direct_ref_ms;
static int64_t ref_failure_ms; /* last REF from the master with REF_MODE_FAILURE, 0 none */
static int64_t ref_ok_ms;      /* last REF from the master without it */

static void agent_rx(const struct raft_rx *it, uint32_t now)
{
	uint16_t id = it->f.id;
	uint8_t zone = (uint8_t)msg_sender(id, it->f.data);

	raft_heard(&R, zone, it->bus, it->relayed, now);
	if (is_hb(id)) {
		return; /* liveness only (raft_heard above); the relay choice reads monitor.c */
	}
	if (raft_base(id) != ID_REF_BASE) {
		if (it->first) {
			raft_on_frame(&R, id, it->f.data, now);
		}
		return;
	}
	if (it->first) {
		raft_on_ref(&R, zone, it->f.data, now);
	}
	if (!raft_ref_is_master(&R, zone, it->f.data)) {
		return;
	}
	/* relayed copies are later by the relay's latency: use them only when cut off */
	if (it->relayed && now - last_direct_ref_ms < 3 * RAFT_CONF_HEARTBEAT_MS) {
		return;
	}
	if (!it->relayed) {
		last_direct_ref_ms = now;
	}
	struct raft_ref m;

	ref_decode(it->f.data, R.term, &m);
	if (m.mode & REF_MODE_FAILURE) {
		ref_failure_ms = now;
	} else {
		ref_ok_ms = now;
	}
	k_spinlock_key_t key = k_spin_lock(&ts_lock);
	bool used = ts_on_ref(&TS, it->bus, m.cycle, m.followup, it->relayed, m.time_us, it->rx_us);
	int64_t g = ts_global(&TS, local_us());

	k_spin_unlock(&ts_lock, key);
	if (used) {
		set_realtime(g);
	}
}

/* ---------------------------------------------------------------- REF_TX */

static atomic_t ref_cycle_sent[2];  /* cycle whose TX-done we last saw, per bus */
static int64_t ref_done_local[2];
static uint8_t ref_cycle;
static atomic_t ref_pending_cycle;

static void ref_done(int bus)
{
	int64_t t = local_us(); /* end of frame on this bus (driver thread) */
	k_spinlock_key_t key = k_spin_lock(&ts_lock); /* 64-bit: no torn reads on the M4 */

	ref_done_local[bus] = t;
	atomic_set(&ref_cycle_sent[bus], atomic_get(&ref_pending_cycle));
	k_spin_unlock(&ts_lock, key);
}

static void ref_task(void *p1, void *p2, void *p3)
{
	static atomic_t seq;
	int64_t next = k_uptime_get();

	while (1) {
		next += RAFT_CONF_HEARTBEAT_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));

		uint32_t term;
		uint8_t mode;

		k_mutex_lock(&raft_lock, K_FOREVER);
		bool lead = raft_ref_info(&R, &term, &mode);
		k_mutex_unlock(&raft_lock);
		if (!lead) {
			continue;
		}
		uint8_t d[2][MSG_DLC];

		for (int bus = 0; bus < 2; bus++) {
			/* follow-up time of the previous REF on this bus, if it made it out */
			k_spinlock_key_t key = k_spin_lock(&ts_lock);
			bool fu = atomic_get(&ref_cycle_sent[bus]) == (uint8_t)(ref_cycle - 1);
			int64_t t = ts_global(&TS, fu ? ref_done_local[bus] : local_us());

			k_spin_unlock(&ts_lock, key);
			struct raft_ref m = { .term = term, .cycle = ref_cycle, .time_us = (uint64_t)t,
					      .mode = mode, .followup = fu };

			ref_encode(&m, d[bus]);
		}
		atomic_set(&ref_pending_cycle, ref_cycle);
		send_ref(ID_REF(MY_ZONE), &seq, d, ref_done);
		ref_cycle++;
	}
}

K_THREAD_DEFINE(ref_tid, 1024, ref_task, NULL, NULL, NULL, PRIO_REF, 0, SYS_FOREVER_MS);

/* ---------------------------------------------------------------- agent thread */

bool raft_agent_failure_mode(void)
{
	return atomic_get(&failure_mode);
}

uint8_t raft_agent_hb_sync_bits(void)
{
	return (uint8_t)atomic_get(&sync_bits);
}

static void agent_tick(uint32_t now)
{
	uint8_t e;
	/* the role at the last tick: we can also become leader in agent_rx (a vote reply) */
	static uint8_t was = RAFT_BOOT;

	while (k_msgq_get(&event_q, &e, K_NO_WAIT) == 0) {
		raft_report(&R, e);
	}
	raft_tick(&R, now);
	if (R.role == ROLE_LEADER && was != ROLE_LEADER) {
		k_spinlock_key_t key = k_spin_lock(&ts_lock);

		ts_become_master(&TS, local_us()); /* carry on from our synced clock: no jump */
		k_spin_unlock(&ts_lock, key);
	}
	k_spinlock_key_t key = k_spin_lock(&ts_lock);

	ts_check(&TS, local_us());
	bool locked = TS.locked, drift = TS.syncs > 2 * TS_BASELINE_US / 1000 / RAFT_CONF_HEARTBEAT_MS;

	k_spin_unlock(&ts_lock, key);

	bool lead = R.role == ROLE_LEADER;
	bool sync = lead || locked;

	was = R.role;

	/* failure mode: the committed log, or sooner the master's REF bit (all at once) */
	bool fail = !R.net.cockpit_ok;

	if (!lead && R.leader != ZONE_NONE) {
		if (ref_failure_ms && now - ref_failure_ms < 100 && ref_failure_ms >= ref_ok_ms) {
			fail = true;
		} else if (ref_ok_ms && now - ref_ok_ms < 100 && ref_ok_ms > ref_failure_ms) {
			fail = false;
		}
	}
	atomic_set(&failure_mode, fail);
	atomic_set(&sync_bits, (sync ? SYNC_LOCKED : 0) | (drift ? SYNC_DRIFT : 0));
	bool hb_both[4] = { false };

	for (int x = ZONE_N1; x <= ZONE_N3; x++) {
		hb_both[x] = x != MY_ZONE && monitor_hb_both(x);
	}
	atomic_set(&relay_zone, route_relay(&R.net, hb_both, MY_ZONE, monitor_sending_hb()));
	gpio_pin_set_dt(&leader_led, lead);
	gpio_pin_set_dt(&sync_led, sync);

	/* who leads, on every change (and the first time it is known) */
	static uint8_t shown_leader = 0xFF, shown_role = 0xFF;
	static uint32_t shown_term;

	if (R.leader != shown_leader || R.term != shown_term || R.role != shown_role) {
		agent_printf("[%u] leader: %s (term %u), this node %s", now,
			     raft_node_name(R.leader), R.term, raft_role_name(R.role));
		shown_leader = R.leader;
		shown_term = R.term;
		shown_role = R.role;
	}
}

static void agent_task(void *p1, void *p2, void *p3)
{
	struct raft_rx it;
	int64_t next = k_uptime_get();

	while (1) {
		int64_t wait = next - k_uptime_get();

		if (wait > 0 && k_msgq_get(&agent_q, &it, K_MSEC(wait)) == 0) {
			k_mutex_lock(&raft_lock, K_FOREVER);
			agent_rx(&it, (uint32_t)k_uptime_get());
			while (k_msgq_get(&agent_q, &it, K_NO_WAIT) == 0) {
				agent_rx(&it, (uint32_t)k_uptime_get());
			}
			k_mutex_unlock(&raft_lock);
			continue;
		}
		k_mutex_lock(&raft_lock, K_FOREVER);
		agent_tick((uint32_t)k_uptime_get());
		k_mutex_unlock(&raft_lock);
		next += RAFT_TICK_MS;
		if (next < k_uptime_get()) {
			next = k_uptime_get() + RAFT_TICK_MS; /* fell behind (a long printout): resync */
		}
	}
}

K_THREAD_DEFINE(agent_tid, 3072, agent_task, NULL, NULL, NULL, PRIO_RAFT, 0, SYS_FOREVER_MS);

int raft_agent_init(void)
{
	if (gpio_pin_configure_dt(&leader_led, GPIO_OUTPUT_INACTIVE) ||
	    gpio_pin_configure_dt(&sync_led, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	struct raft_io io = { .send = io_send, .applied = io_applied, .log = io_log };

	ts_init(&TS);
	raft_init(&R, MY_ZONE, &io, (uint32_t)k_uptime_get());
	printk("raft: %s, default leader %d, seed %u, log %d entries\n",
	       ((const char *const[])RAFT_CONF_NODE_NAMES)[MY_ZONE], RAFT_CONF_DEFAULT_LEADER,
	       RAFT_CONF_SEED, RAFT_CONF_LOG_SIZE);
	k_thread_start(print_tid);
	k_thread_start(agent_tid);
	k_thread_start(ref_tid);
	return 0;
}
