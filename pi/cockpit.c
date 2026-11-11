/*
 * Cockpit zone (N1). Wheel state over UDP -> CTRL and BLINK on both CAN buses,
 * HB_N1 every 20 ms, heartbeats / FAULT / FORCE from the other zones, error state.
 *
 * Raspberry Pi 5. Needs lgpio: sudo apt install liblgpio-dev
 * Build: gcc -O2 -Wall -o cockpit cockpit.c -lpthread -llgpio
 * Run:   sudo ./cockpit      (sudo for SCHED_FIFO)
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <lgpio.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../common/can_msg.h"
#include "dijoystate.h"
#include "raft_node.h"

#define UDP_PORT     8000 /* wheel state in */
#define FORCE_PORT   8001 /* force byte back to whoever sent the wheel state */
#define WHEEL_ON_MAC 1    /* 1: mac proxy pedals 0 (up)..32767 (down); 0: windows 32767 (up)..-32768 (down) */

/* thresholds, all in one place */
#define HB_PERIOD_MS     20
#define HB_TIMEOUT_MS    65  /* 3 missed heartbeats + margin (R4.5) */
#define DEG_TIMEOUT_MS   45  /* DEG on within 60 ms of a pull (R4.7): 45 + one 10 ms check */
#define MON_PERIOD_MS    10
#define TURN_CANCEL_DEG  150 /* turn signal cancels after the wheel passes this and comes back */
#define DOUBLE_PRESS_MS  500
#define FORCE_LIMIT      20  /* force is a dummy counter: keep the wheel from jerking */
#define TX_PROBE_MS      100 /* send rate on a bus we hear nobody on */
#define UDP_TIMEOUT_MS   100 /* no wheel update this long: cockpit treats itself as failed (lab 1 reqs) */
#define RELAY_DETECT_MS  30  /* relay a zone's frames once it is missing this long on a bus */
#define RAFT_LOG_FILE    "raft_cockpit.log" /* term, vote and entries; next to where it runs */

/* test points (BCM numbers); must not be the MCP2515 INT pins in config.txt */
#define PIN_UDP_RX 22 /* header pin 15 */
#define PIN_CMD_TX 27 /* pin 13 */
#define PIN_TX_A   5  /* pin 29 */
#define PIN_TX_B   6  /* pin 31 */
#define PIN_DEG_A  13 /* pin 33 */
#define PIN_DEG_B  16 /* pin 36 */

/* proxy button indices */
#define WB_TEST  1 /* B: cockpit test button */
#define WB_BAD   3 /* Y: send one out-of-range CTRL (bad-input demo) */
#define WB_RIGHT 4
#define WB_LEFT  5

static int64_t now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void sleep_until_ms(int64_t ms)
{
	struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {
	}
}

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* 0 means never */
static int recent(int64_t t, int64_t now)
{
	return t > 0 && now - t < HB_TIMEOUT_MS;
}

/* GPIO test points through lgpio; the Pi 5 header GPIOs are on the RP1 chip */
static int gpio_h = -1;

static void gpio_init(void)
{
	const int pins[] = { PIN_UDP_RX, PIN_CMD_TX, PIN_TX_A, PIN_TX_B, PIN_DEG_A, PIN_DEG_B,
			     PIN_PPS, PIN_LEADER, PIN_SYNC, PIN_REF_TX };

	/* the header GPIOs are on the chip labelled pinctrl-*: pinctrl-rp1 on a Pi 5
	 * (gpiochip0, or gpiochip4 on older kernels), pinctrl-bcm2711 on a Pi 4 */
	for (int n = 0; n <= 4 && gpio_h < 0; n++) {
		lgChipInfo_t info;
		int h = lgGpiochipOpen(n);

		if (h < 0) {
			continue;
		}
		if (lgGpioGetChipInfo(h, &info) >= 0 && strncmp(info.label, "pinctrl-", 8) == 0) {
			gpio_h = h;
		} else {
			lgGpiochipClose(h);
		}
	}
	if (gpio_h < 0) {
		fprintf(stderr, "no pinctrl gpiochip found, run gpiodetect (test points off)\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		lgGpioClaimOutput(gpio_h, 0, pins[i], 0);
	}
}

static void gpio_put(int pin, int v)
{
	if (gpio_h >= 0) {
		lgGpioWrite(gpio_h, pin, v);
	}
}

/* CAN */
static int can_fd[2];
static const int tx_pin[2] = { PIN_TX_A, PIN_TX_B };
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

static int can_open(const char *ifname)
{
	struct sockaddr_can addr = { .can_family = AF_CAN, .can_ifindex = if_nametoindex(ifname) };
	int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);

	if (fd < 0 || addr.can_ifindex == 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror(ifname);
		exit(1);
	}
	/* the catalog's filters (all of Raft, the heartbeats, FORCE); the EFF flag in the mask
	 * keeps extended frames out */
#define FILTER(i, m) { (i), (m) | CAN_EFF_FLAG },
	struct can_filter f[] = { CAN_RX_FILTERS(FILTER) };
#undef FILTER
	setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, f, sizeof(f));
	/* Lab 5: our own frames come back (MSG_CONFIRM) once the controller has sent them,
	 * which is the TX-done time for the REF follow-up; and kernel receive timestamps */
	int one = 1;
	setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &one, sizeof(one));
	setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof(one));
	/* a dead bus fails its write right away instead of blocking the other bus */
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

static int skip_bus(int bus, int64_t now);

/* queue one frame on both buses back to back; seq goes up once per message, not per bus */
/* mark: a test pin raised right after the first write returns (CMD_TX for CTRL), or -1 */
static void send_both(uint16_t id, uint8_t *seq, uint8_t d[MSG_DLC], int mark)
{
	struct can_frame f = { .can_id = id, .can_dlc = MSG_DLC };

	int64_t now = now_ms();

	pthread_mutex_lock(&tx_lock);
	d[0] = (d[0] & 0xF0) | (*seq & 0x0F);
	(*seq)++;
	memcpy(f.data, d, MSG_DLC);
	for (int bus = 0; bus < 2; bus++) {
		if (skip_bus(bus, now)) {
			continue;
		}
		gpio_put(tx_pin[bus], 1);
		if (write(can_fd[bus], &f, sizeof(f)) < 0) {
			/* dead or full bus: the other bus still carries this frame */
		}
		gpio_put(tx_pin[bus], 0);
		if (mark >= 0) {
			gpio_put(mark, 1);
			mark = -1;
		}
	}
	pthread_mutex_unlock(&tx_lock);
}

/* Raft frames: seq already set by raft_node */
static void send_raw_both(uint16_t id, uint8_t d[MSG_DLC])
{
	struct can_frame f = { .can_id = id, .can_dlc = MSG_DLC };
	int64_t now = now_ms();

	memcpy(f.data, d, MSG_DLC);
	pthread_mutex_lock(&tx_lock);
	for (int bus = 0; bus < 2; bus++) {
		if (!skip_bus(bus, now) && write(can_fd[bus], &f, sizeof(f)) < 0) {
			/* dead or full bus: the other bus still carries it */
		}
	}
	pthread_mutex_unlock(&tx_lock);
}

/* one copy on one bus: REF (per-bus follow-up time) and relayed copies */
static void send_bus(int bus, uint16_t id, const uint8_t d[MSG_DLC])
{
	struct can_frame f = { .can_id = id, .can_dlc = MSG_DLC };

	memcpy(f.data, d, MSG_DLC);
	pthread_mutex_lock(&tx_lock);
	if (!skip_bus(bus, now_ms()) && write(can_fd[bus], &f, sizeof(f)) < 0) {
	}
	pthread_mutex_unlock(&tx_lock);
}

/* shared between the threads, guarded by lock; times in ms, 0 = never */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int64_t hb_ms[4][2];  /* [zone][bus] last heartbeat */
static int64_t any_ms[2];    /* [bus] last frame from any other zone */
static int64_t direct_ms[4][2]; /* [zone][bus] last frame heard straight from it (relay) */
static int64_t hb_direct_ms[4][2]; /* [zone][bus] last heartbeat heard straight from it */
static int in_error = 1;     /* power up in error (R3) */
static int self_test;        /* wheel test button has failed this zone */
static int udp_since_error;  /* a valid wheel update arrived since the error began */
static int64_t udp_ms;       /* last wheel update, 0 = never */
static int deg[2];
static int bl_state = BLINK_HAZARD, bl_passed; /* goes out in CTRL b7 (Lab 5) */
static uint8_t bl_slot;      /* turn start slot (CTRL b7 bits 2-7) */
static struct sockaddr_in proxy_addr; /* where force goes, learned from the wheel packets */
static int have_proxy;
static int zone_err[4];      /* [zone] error state from its heartbeat */

static int force_fd;

/*
 * tx_lock held. A bus we hear nobody on gets one probe every TX_PROBE_MS instead of
 * every frame: a half-connected transceiver otherwise keeps jamming that bus, and the
 * retries tie up the SPI bus the other CAN chip shares. A probe that gets answered
 * clears DEG within one HB_MON tick, so recovery stays well inside 100 ms.
 */
static int skip_bus(int bus, int64_t now)
{
	static int64_t probe_at[2];

	pthread_mutex_lock(&lock);
	int quiet = deg[bus];
	pthread_mutex_unlock(&lock);

	if (!quiet) {
		return 0;
	}
	if (now < probe_at[bus]) {
		return 1;
	}
	probe_at[bus] = now + TX_PROBE_MS;
	return 0;
}

/* lock held */
static void set_blink(int s)
{
	if (bl_state == s) {
		return;
	}
	bl_state = s;
	bl_passed = 0;
	if (s == BLINK_LEFT || s == BLINK_RIGHT) {
		/* start on the first 50 ms grid point at least 10 ms ahead: every STM32 has the
		 * frame by then, relayed or not, and it is still within R2.4's 100 ms */
		int64_t start = (pi_global_gettime() + 10000) / TURN_GRID_US + 1;

		bl_slot = (uint8_t)(start & 0x3F);
	}
}

/* lock held: a turn signal cancels once the wheel has gone past the threshold and come back */
static void turn_cancel(int steer)
{
	int past = (bl_state == BLINK_LEFT && steer < -TURN_CANCEL_DEG) ||
		   (bl_state == BLINK_RIGHT && steer > TURN_CANCEL_DEG);

	if (past) {
		bl_passed = 1;
	} else if (bl_passed && (bl_state == BLINK_LEFT || bl_state == BLINK_RIGHT)) {
		set_blink(BLINK_OFF);
	}
}

/* lock held; returns 1 if this started a new error, so the caller sends FAULT */
static int enter_error(void)
{
	if (in_error) {
		return 0;
	}
	in_error = 1;
	udp_since_error = 0;
	set_blink(BLINK_HAZARD);
	return 1;
}

static void send_fault(int zone, int reason)
{
	static uint8_t seq;
	uint8_t d[MSG_DLC] = { 0, zone, reason, ZONE_N1 };

	send_both(ID_FAULT, &seq, d, -1);
	printf("FAULT sent: zone %d reason %d\n", zone, reason);
}

/* HB_TX: 20 ms */
static void *hb_tx_thread(void *arg)
{
	uint8_t seq = 0;
	int64_t next = now_ms();

	(void)arg;
	for (;;) {
		next += HB_PERIOD_MS;
		sleep_until_ms(next);

		pthread_mutex_lock(&lock);
		int64_t now = now_ms();
		/* stream lost: stop the heartbeat so N2/N3 see the cockpit as failed */
		int quiet = self_test || !(udp_ms > 0 && now - udp_ms < UDP_TIMEOUT_MS);
		uint8_t d[MSG_DLC] = { 0, in_error ? STATE_ERROR : STATE_NORMAL,
				       (deg[0] ? HEALTH_A_DEG : 0) | (deg[1] ? HEALTH_B_DEG : 0),
				       raft_node_sync_bits() };
		pthread_mutex_unlock(&lock);

		if (!quiet) {
			send_both(ID_HB_N1, &seq, d, -1); /* silence is how the others see the test button */
		}
	}
	return NULL;
}

/* HB_MON: zone liveness, degraded LEDs, error state */
static void *mon_thread(void *arg)
{
	int shown = -1;
	int64_t next = now_ms();

	(void)arg;
	for (;;) {
		next += MON_PERIOD_MS;
		sleep_until_ms(next);
		int64_t now = now_ms();

		pthread_mutex_lock(&lock);
		int n2 = recent(hb_ms[ZONE_N2][0], now) || recent(hb_ms[ZONE_N2][1], now);
		int n3 = recent(hb_ms[ZONE_N3][0], now) || recent(hb_ms[ZONE_N3][1], now);
		int lost = !n2 ? ZONE_N2 : (!n3 ? ZONE_N3 : 0);
		int udp_ok = udp_ms > 0 && now - udp_ms < UDP_TIMEOUT_MS;

		for (int b = 0; b < 2; b++) {
			deg[b] = !(any_ms[b] > 0 && now - any_ms[b] < DEG_TIMEOUT_MS);
		}
		int fault = lost && enter_error();
		int udp_fault = !lost && !udp_ok && enter_error();
		if (in_error && !lost && udp_ok && !self_test && udp_since_error) {
			in_error = 0;
			set_blink(BLINK_OFF);
		}
		/* lead the hazards whenever any zone is in error, so front and rear follow one
		 * pattern instead of each blinking on its own timer */
		int zerr = zone_err[ZONE_N2] || zone_err[ZONE_N3];
		if (!in_error && zerr) {
			set_blink(BLINK_HAZARD);
		} else if (!in_error && bl_state == BLINK_HAZARD) {
			set_blink(BLINK_OFF);
		}
		int err = in_error, d0 = deg[0], d1 = deg[1];
		pthread_mutex_unlock(&lock);

		gpio_put(PIN_DEG_A, d0);
		gpio_put(PIN_DEG_B, d1);
		if (fault) {
			send_fault(lost, FAULT_HB_LOST);
		}
		if (udp_fault) {
			send_fault(ZONE_N1, FAULT_CTRL_LOST);
		}

		int state = err | n2 << 1 | n3 << 2 | d0 << 3 | d1 << 4;
		if (state != shown) { /* print on change only */
			printf("%s n2=%d n3=%d degA=%d degB=%d\n", err ? "ERROR " : "NORMAL", n2, n3, d0, d1);
			shown = state;
		}
	}
	return NULL;
}

/* hooks for raft_node */
static uint8_t hook_deg_bits(void)
{
	pthread_mutex_lock(&lock);
	uint8_t d = (deg[0] ? HEALTH_A_DEG : 0) | (deg[1] ? HEALTH_B_DEG : 0);
	pthread_mutex_unlock(&lock);
	return d;
}

static bool hook_hb_both(uint8_t zone)
{
	int64_t now = now_ms();

	pthread_mutex_lock(&lock);
	bool on = true;

	for (int b = 0; b < 2; b++) {
		on &= hb_direct_ms[zone][b] > 0 && now - hb_direct_ms[zone][b] < RELAY_DETECT_MS;
	}
	pthread_mutex_unlock(&lock);
	return on;
}

static bool hook_sending_hb(void)
{
	int64_t now = now_ms();

	pthread_mutex_lock(&lock);
	bool on = !self_test && udp_ms > 0 && now - udp_ms < UDP_TIMEOUT_MS;
	pthread_mutex_unlock(&lock);
	return on;
}

/* FORCE_RX, heartbeat reception, Raft frames and RELAY, both buses */
static struct dedup dd[MSG_COUNT];

static void rx_frame(int bus, const struct can_frame *f, int64_t rx_us)
{
	if ((f->can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) || f->can_dlc != MSG_DLC) {
		return;
	}
	uint16_t id = f->can_id & CAN_SFF_MASK;
	int idx = msg_index(id), zone = msg_sender(id, f->data);

	if (idx < 0 || zone < ZONE_N1 || zone > ZONE_N3) {
		return;
	}

	int64_t now = now_ms();
	bool relayed = f->data[0] & FLAG_RELAYED;
	pthread_mutex_lock(&lock);
	any_ms[bus] = now; /* both copies count: they show the bus works */
	if (is_hb(id)) {
		hb_ms[zone][bus] = now;
		zone_err[zone] = f->data[1] == STATE_ERROR;
	}
	if (!relayed) {
		direct_ms[zone][bus] = now;
		if (is_hb(id)) {
			hb_direct_ms[zone][bus] = now;
		}
	}
	int other = 1 - bus;
	bool relay = !relayed && raft_node_relay() == ZONE_N1 && !deg[other] &&
		     !(direct_ms[zone][other] > 0 && now - direct_ms[zone][other] < RELAY_DETECT_MS);
	struct sockaddr_in to = proxy_addr;
	int have = have_proxy;
	pthread_mutex_unlock(&lock);

	bool copy = dedup_is_copy(&dd[idx], SEQ(f->data), bus);

	/* Lab 5: the Pi relays too, as the last resort when neither STM32 is on both buses
	 * (route.h):
	 * forward-on-detect, the copy marked FLAG_RELAYED so nobody forwards it again */
	if (!copy && relay) {
		uint8_t d[MSG_DLC];

		memcpy(d, f->data, MSG_DLC);
		d[0] |= FLAG_RELAYED;
		send_bus(other, (uint16_t)id, d);
	}
	if (is_raft(id) || is_hb(id)) {
		raft_node_rx(bus, (uint16_t)id, f->data, relayed, !copy, rx_us);
	}
	if (copy) {
		return;
	}
	if (id == ID_FORCE && have) {
		int8_t force = (int8_t)clamp((int8_t)f->data[1], -FORCE_LIMIT, FORCE_LIMIT);

		sendto(force_fd, &force, 1, MSG_DONTWAIT, (struct sockaddr *)&to, sizeof(to));
	} else if (id == ID_FAULT) {
		printf("FAULT from zone %d: zone %d reason %d\n", f->data[3], f->data[1], f->data[2]);
	}
}

static void *rx_thread(void *arg)
{
	struct pollfd pfd[2] = { { can_fd[0], POLLIN, 0 }, { can_fd[1], POLLIN, 0 } };

	(void)arg;
	for (;;) {
		poll(pfd, 2, -1);
		for (int bus = 0; bus < 2; bus++) {
			struct can_frame f;
			char ctl[CMSG_SPACE(sizeof(struct timespec))];
			struct iovec iov = { &f, sizeof(f) };
			struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = ctl,
					    .msg_controllen = sizeof(ctl) };

			/* read everything waiting; a failed read also clears a socket error */
			while (recvmsg(can_fd[bus], &m, 0) == (ssize_t)sizeof(f)) {
				int64_t rx_us = pi_local_us();
				struct cmsghdr *c = CMSG_FIRSTHDR(&m);

				/* the kernel's receive time is CLOCK_REALTIME: move it onto our clock */
				if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
					struct timespec k, nowr;

					memcpy(&k, CMSG_DATA(c), sizeof(k));
					clock_gettime(CLOCK_REALTIME, &nowr);
					rx_us -= (nowr.tv_sec - k.tv_sec) * 1000000LL +
						 (nowr.tv_nsec - k.tv_nsec) / 1000;
				}
				if (m.msg_flags & MSG_CONFIRM) {
					/* our own frame, now on the wire: only REF's time matters */
					if ((f.can_id & CAN_SFF_MASK) == ID_REF(ZONE_N1)) {
						raft_node_ref_echo(bus, f.data, rx_us);
					}
				} else {
					rx_frame(bus, &f, rx_us);
				}
				m.msg_controllen = sizeof(ctl);
				m.msg_flags = 0;
			}
		}
		/* a downed interface keeps poll() returning at once; at SCHED_FIFO that spin
		 * would starve the whole Pi, so back off 1 ms */
		if ((pfd[0].revents | pfd[1].revents) & (POLLERR | POLLHUP | POLLNVAL)) {
			sleep_until_ms(now_ms() + 1);
		}
	}
	return NULL;
}

/* can0/can1 are numbered in probe order, not by chip select; bus A is the chip on CE0 (spi0.0) */
static int on_ce0(const char *ifname)
{
	char path[64], link[128];

	snprintf(path, sizeof(path), "/sys/class/net/%s/device", ifname);
	ssize_t n = readlink(path, link, sizeof(link) - 1);
	if (n < 0) {
		return 0;
	}
	link[n] = '\0';
	return strstr(link, "spi0.0") != NULL;
}

static uint8_t pedal_pct(int v)
{
#if WHEEL_ON_MAC
	return clamp(v, 0, 32767) * 100 / 32767;
#else
	return (32767 - clamp(v, -32768, 32767)) * 100 / 65535;
#endif
}

int main(void)
{
	/* real-time priority and locked memory so the brake path isn't preempted or paged;
	 * kept below the kernel's irq threads (50) so the CAN driver always gets to run */
	mlockall(MCL_CURRENT | MCL_FUTURE);
	struct sched_param sp = { .sched_priority = 40 };
	if (sched_setscheduler(0, SCHED_FIFO, &sp)) {
		perror("SCHED_FIFO (continuing without it)");
	}

	gpio_init();
	const char *bus_a = on_ce0("can1") ? "can1" : "can0";
	const char *bus_b = strcmp(bus_a, "can0") == 0 ? "can1" : "can0";

	printf("bus A = %s (CE0), bus B = %s (CE1)\n", bus_a, bus_b);
	can_fd[0] = can_open(bus_a);
	can_fd[1] = can_open(bus_b);
	any_ms[0] = any_ms[1] = now_ms(); /* short grace period before DEG judges the buses */

	/* listen on every interface, wifi or ethernet */
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(UDP_PORT),
				    .sin_addr.s_addr = htonl(INADDR_ANY) };
	int udp = socket(AF_INET, SOCK_DGRAM, 0);
	force_fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (udp < 0 || force_fd < 0 || bind(udp, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		perror("udp");
		return 1;
	}

	struct raft_node_hooks hooks = { .send_both = send_raw_both, .send_bus = send_bus,
					 .gpio = gpio_put, .deg_bits = hook_deg_bits,
					 .sending_hb = hook_sending_hb, .hb_both = hook_hb_both };
	const char *log_file = getenv("RAFT_LOG");

	raft_node_init(&hooks, log_file ? log_file : RAFT_LOG_FILE);

	pthread_t t;
	pthread_create(&t, NULL, rx_thread, NULL);
	pthread_create(&t, NULL, hb_tx_thread, NULL);
	pthread_create(&t, NULL, mon_thread, NULL);
	raft_node_start();

	/* UDP_RX -> CTRL_TX: one CTRL per wheel update (R4.4) */
	uint8_t ctrl_seq = 0;
	uint8_t pkt[4 + sizeof(DIJOYSTATE2_t)]; /* 4-byte counter + wheel state */
	int prev_left = 0, prev_right = 0, prev_test = 0, prev_bad = 0;
	int64_t last_press = 0;
	int64_t last_pkt = 0, max_gap = 0, report_at = now_ms() + 1000;
	int pkts = 0;

	for (;;) {
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		ssize_t n = recvfrom(udp, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fromlen);

		gpio_put(PIN_UDP_RX, 1);
		if (n != (ssize_t)sizeof(pkt)) {
			gpio_put(PIN_UDP_RX, 0);
			continue;
		}
		DIJOYSTATE2_t w;
		memcpy(&w, pkt + 4, sizeof(w));

		int left = w.rgbButtons[WB_LEFT] != 0, right = w.rgbButtons[WB_RIGHT] != 0;
		int test = w.rgbButtons[WB_TEST] != 0, bad = w.rgbButtons[WB_BAD] != 0;
		struct ctrl c = {
			.steer = (int16_t)clamp((int)((long long)w.lX * STEER_MAX / 32768), STEER_MIN, STEER_MAX),
			.throttle = pedal_pct(w.lY),
			.brake = pedal_pct(w.lRz),
			.buttons = (left ? BTN_LEFT : 0) | (right ? BTN_RIGHT : 0) | (test ? BTN_TEST : 0),
		};
		if (bad && !prev_bad) {
			c.throttle = 200; /* one out-of-range CTRL */
		}
		pthread_mutex_lock(&lock);
		c.blink = (uint8_t)bl_state; /* Lab 5: the turn state rides in CTRL b7 */
		c.turn_slot = bl_slot;
		pthread_mutex_unlock(&lock);

		uint8_t d[MSG_DLC] = { 0 };
		ctrl_encode(&c, d);
		/* CMD_TX rises right after the first CAN write returns: the Pi stage's stop edge */
		send_both(ID_CTRL, &ctrl_seq, d, PIN_CMD_TX);
		gpio_put(PIN_CMD_TX, 0);

		/* bookkeeping after the frame is out, so it stays off the brake path */
		int64_t now = now_ms();
		int fault = 0, pressed = 0;

		pthread_mutex_lock(&lock);
		proxy_addr = from;
		proxy_addr.sin_port = htons(FORCE_PORT);
		have_proxy = 1;
		udp_since_error = 1;
		udp_ms = now;
		if (left && !prev_left && bl_state != BLINK_HAZARD) {
			set_blink(BLINK_LEFT);
		}
		if (right && !prev_right && bl_state != BLINK_HAZARD) {
			set_blink(BLINK_RIGHT);
		}
		turn_cancel(c.steer);
		if (test && !prev_test) {
			if (self_test && now - last_press <= DOUBLE_PRESS_MS) {
				self_test = 0; /* double press clears; HB_MON decides when we leave error */
			} else {
				self_test = 1;
				last_press = now;
				fault = enter_error();
				pressed = 1;
			}
		}
		pthread_mutex_unlock(&lock);

		if (fault) {
			send_fault(ZONE_N1, FAULT_TEST_BTN);
		}
		if (pressed) {
			raft_node_event(APP_TEST_BTN); /* outside `lock`: raft_lock is taken first elsewhere */
		}

		/* once a second: is the wheel stream arriving, and how bursty is the wifi */
		pkts++;
		if (last_pkt && now - last_pkt > max_gap) {
			max_gap = now - last_pkt;
		}
		last_pkt = now;
		if (now >= report_at) {
			printf("wheel: %d pkt/s, max gap %lld ms, steer %d thr %u brk %u btn 0x%02x\n", pkts,
			       (long long)max_gap, c.steer, c.throttle, c.brake, c.buttons);
			pkts = 0;
			max_gap = 0;
			report_at = now + 1000;
		}
		prev_left = left;
		prev_right = right;
		prev_test = test;
		prev_bad = bad;
		gpio_put(PIN_UDP_RX, 0);
	}
}
