// Lab 3, N1 (Raspberry Pi), using the Lab 2 message catalog (can_protocol.h):
// ZONE_CTRL, HB_COCKPIT and BLINKER out on can0 and can1; HB_STEERING, HB_DRIVETRAIN and FORCE_FEEDBACK in.
//
// Build: gcc -O2 -Wall receiver.c -o proxy_receiver
// Run:   ./proxy_receiver [iface_A iface_B]     (default can0 can1; vcan0 vcan1 works for testing)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include "state.h"
#include "can_protocol.h"

/** Configure this **/
#define LOCAL_HOST "172.26.166.20" // IP of local interface
#define R_PORT 8000                // wheel state comes in here

#define REMOTE_HOST "172.26.76.97" // force goes back here
#define S_PORT 8001
#define MAX_FORCE_VAL 100          // force byte sent to the proxy is clamped to +/- this

// set to 1 when using MAC
#define WHEEL_ON_MAC 0

// GPIO settings
#define PIN_UDP_RX 22
#define PIN_CMD_TX 27
#define PIN_TX_A   5
#define PIN_TX_B   6
#define PIN_DEG_A  13  // degraded-bus LEDs
#define PIN_DEG_B  26

#define RX_DEBUG 1  // print every frame we receive (and why a frame was rejected)
#define TX_DEBUG 1  // print every frame we send, per bus
// printing slows the threads: set both to 0 for timing measurements

#define HB_PERIOD_MS      20
#define DEBOUNCE_MS       20    // test button (same as Lab 2)
#define DOUBLE_PRESS_MS   500
#define HB_MISS_MS        55    // a zone/bus is dead after this long with no heartbeat (3 missed + margin)
#define MON_TICK_MS       2     // how often HB_MON checks (must be <= 5 so DEG lights within 60 ms)
#define BLINK_TICK_MS     2     // how often the blink thread checks if it has to send
#define BLINK_TURN_HALF_MS   500 // turn signal: time between edges (1 Hz, R2.4)
#define BLINK_HAZARD_HALF_MS 500 // hazard: time between edges (Lab 2 blinker.c used 250)
#define BLINK_MIN_GAP_MS  20    // never send BLINKER closer together than this
#define LEFT_THRESHOLD    299   // wheel angle 0..900, from Lab 2 blinker.c
#define RIGHT_THRESHOLD   601

static volatile uint32_t *gpio;

static void gpio_init(void) {
  int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
  if (fd < 0) {
    perror("open /dev/gpiomem (test points disabled)");
    return;
  }
  void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) {
    perror("mmap (test points disabled)");
    return;
  }
  gpio = m;

  int pins[] = {PIN_UDP_RX, PIN_CMD_TX, PIN_TX_A, PIN_TX_B, PIN_DEG_A, PIN_DEG_B}; // GPIO pins init
  for (int i = 0; i < (int) (sizeof(pins) / sizeof(pins[0])); i++) {
    int p = pins[i];
    // each pin has a 3-bit mode field, 10 pins per register. Mode 1 = output.
    gpio[p / 10] = (gpio[p / 10] & ~(7 << ((p % 10) * 3))) | (1 << ((p % 10) * 3));
    gpio[10] = 1 << p; // register 10 clears pins (start low)
  }
}

static void gpio_put(int pin, int value) {
  if (gpio) gpio[value ? 7 : 10] = 1 << pin; // register 7 sets pins, register 10 clears them
}

// CAN
static int can_open(const char *ifname) {
  int fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (fd < 0) {
    perror("socket(PF_CAN)");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_can addr = {0};
  addr.can_family = AF_CAN;
  addr.can_ifindex = if_nametoindex(ifname);
  if (addr.can_ifindex == 0) {
    fprintf(stderr, "no such interface: %s\n", ifname);
    exit(EXIT_FAILURE);
  }
  if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
    perror("bind(can)");
    exit(EXIT_FAILURE);
  }

  // only let the kernel hand us the IDs we use (standard frames only)
  // everything else on the bus INT64_MIN wakes us up
  uint32_t mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
  struct can_filter filters[] = {
    {CAN_ID_HB_STEERING, mask}, {CAN_ID_HB_DRIVETRAIN, mask}, {CAN_ID_FORCE_FEEDBACK, mask},
  };
  setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters));

  // Non-blocking: if a bus is dead and its queue is full, write() fails right away
  // instead of freezing us. A dead bus must not stop the other bus.
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

static int can_fd[2]; // [0] = bus A, [1] = bus B
static const int tx_pin[2] = {PIN_TX_A, PIN_TX_B};

// one 8-bit seq counter per message ID
static uint8_t ctrl_seq = 0, hb_seq = 0, blink_seq = 0;

// main thread: ZONE_CTRL
// thread 1: HB_COCKPIT
// thread 2: BLINKER
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

static void dbg_frame(const char *dir, int bus, uint32_t id, const uint8_t *data, int dlc, const char *tag);

// send frame on both buses. CAN hardware adds and checks the CRC, so the payload carries none.
static void can_send_both(uint32_t id, const uint8_t *data, uint8_t dlc) {
  struct can_frame f = {0};
  f.can_id = id;
  f.can_dlc = dlc;
  memcpy(f.data, data, dlc);

  int err[2] = {0, 0};
  pthread_mutex_lock(&tx_lock);
  for (int bus = 0; bus < 2; bus++) {
    gpio_put(tx_pin[bus], 1);
    if (write(can_fd[bus], &f, sizeof(f)) < 0) err[bus] = errno; // bus dead / queue full: we just move on
    gpio_put(tx_pin[bus], 0);
  }
  pthread_mutex_unlock(&tx_lock);

#if TX_DEBUG
  for (int bus = 0; bus < 2; bus++) {
    dbg_frame("tx", bus, id, f.data, dlc, err[bus] ? strerror(err[bus]) : "queued");
  }
#else
  (void) err;
#endif
}

static int clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// time stuff
#define NS_PER_MS  1000000LL
#define NS_PER_SEC 1000000000LL

static int64_t now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t) t.tv_sec * NS_PER_SEC + t.tv_nsec;
}

static void sleep_until(int64_t deadline_ns) {
  struct timespec t = {
    .tv_sec  = deadline_ns / NS_PER_SEC,
    .tv_nsec = deadline_ns % NS_PER_SEC,
  };
  while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == EINTR) {}
}

// one line per frame, built first so lines from different threads don't interleave
__attribute__((unused))
static void dbg_frame(const char *dir, int bus, uint32_t id, const uint8_t *data, int dlc, const char *tag) {
  char buf[192];
  int n = snprintf(buf, sizeof(buf), "%10.3f %s bus%c id=0x%03X dlc=%d data=", now_ns() / 1e6, dir,
                   'A' + bus, (unsigned) id, dlc);
  for (int i = 0; i < dlc && i < 8 && n < (int) sizeof(buf) - 4; i++) {
    n += snprintf(buf + n, sizeof(buf) - n, "%02X ", data[i]);
  }
  snprintf(buf + n, sizeof(buf) - n, "%s\n", tag);
  fputs(buf, stdout);
}

// STATE TRACKING
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

// ERROR STATE: we power up in error until both peers' heartbeats and a valid UDP update show up
static uint8_t health = 0; // bit0 = bus A degraded, bit1 = bus B
static int self_test = 0; // set while the wheel test button has failed this zone
static int in_err = 1;       // are we in the error state
static int udp_valid = 0;    // a valid UDP update arrived since the error began

// RX TRACKING 
static int64_t hb_seen[2][2]; // [bus][peer], peer 0 = N2, 1 = N3. when we last heard its HB
static int64_t peer_frame[2]; // [bus] when we last heard ANY peer frame on that bus

#define DEDUP_MS 100
enum { K_HB2, K_HB3, K_FORCE, K_N };
static struct { uint8_t seq; int64_t t; } dd[K_N]; // last accepted seq and arrival time per key

// UDP socket to send the force byte back to the proxy
static int force_fd;
static struct sockaddr_in force_addr;

// BLINKERS
static int bl_state = BLINK_HAZARD; // starts in hazard
static int bl_phase = 1; // 1 for LED on
static int bl_dirty = 1; // 1 = state changed, a BLINKER must go out (starts 1: announce it)
static int bl_passed = 0; // wheel went past the threshold; turn cancels when it comes back
static int64_t bl_next_toggle; // when the next phase flip is due
static int64_t bl_last_tx = INT64_MIN;

static int64_t bl_half_ns(void) {
  return (bl_state == BLINK_HAZARD ? BLINK_HAZARD_HALF_MS : BLINK_TURN_HALF_MS) * NS_PER_MS;
}

static void bl_set(int s) {
  if (bl_state == s) return;
  bl_state = s;
  bl_passed = 0;
  bl_phase = (s == BLINK_OFF) ? 0 : 1; // a change restarts the pattern with lamps on
  bl_dirty = 1; // and the blink thread sends it right away
}

static void bl_left(void)  { if (bl_state != BLINK_HAZARD) bl_set(BLINK_LEFT); }  // hazards win
static void bl_right(void) { if (bl_state != BLINK_HAZARD) bl_set(BLINK_RIGHT); }

static void bl_hazard(int on) {
  if (on) bl_set(BLINK_HAZARD);
  else if (bl_state == BLINK_HAZARD) bl_set(BLINK_OFF);
}

// Cancel the turn signal once the wheel goes past the threshold and comes back.
static void bl_steer(int angle) {
  if (bl_state == BLINK_LEFT) {
    if (angle < LEFT_THRESHOLD) bl_passed = 1;
    else if (bl_passed) bl_set(BLINK_OFF);
  } else if (bl_state == BLINK_RIGHT) {
    if (angle > RIGHT_THRESHOLD) bl_passed = 1;
    else if (bl_passed) bl_set(BLINK_OFF);
  }
}

// enter err state
static void enter_error(void) {
  if (!in_err) {
    in_err = 1;
    udp_valid = 0;  // need a fresh valid UDP update before we may leave again
    bl_hazard(1);
  }
}

// HB TX thread, every 20ms
static void *hb_thread(void *arg) {
  (void) arg;
  int64_t next = now_ns();

  while (1) {
    next += HB_PERIOD_MS * NS_PER_MS;
    sleep_until(next);

    pthread_mutex_lock(&state_lock);
    int silent = self_test;  // while we are in our own fault, send no heartbeat
    uint8_t h = health;
    pthread_mutex_unlock(&state_lock);
    if (silent) continue;

    struct heartbeat hb = {
      .can_a_degraded = h & 1,
      .can_b_degraded = (h >> 1) & 1,
      .seq = hb_seq++,
    };
    uint8_t data[CAN_DLC_HB];
    hb_encode(&hb, data);
    can_send_both(CAN_ID_HB_COCKPIT, data, CAN_DLC_HB);
  }
  return NULL;
}

// BLINKER TX thread, wakes every 2 ms
static void *blink_thread(void *arg) {
  (void) arg;
  int64_t next = now_ns();

  pthread_mutex_lock(&state_lock);
  bl_next_toggle = now_ns() + bl_half_ns();
  pthread_mutex_unlock(&state_lock);

  while (1) {
    next += BLINK_TICK_MS * NS_PER_MS;
    sleep_until(next);
    int64_t now = now_ns();

    pthread_mutex_lock(&state_lock);
    int send = 0;
    if (bl_dirty && now - bl_last_tx >= BLINK_MIN_GAP_MS * NS_PER_MS) {
      send = 1;                                    // state changed
    } else if (!bl_dirty && now >= bl_next_toggle) {
      if (bl_state != BLINK_OFF) bl_phase ^= 1;    // half period passed: flip lamps on/off
      send = 1;
    }
    struct blinker_msg m = { .state = bl_state, .phase = bl_phase };
    if (send) {
      bl_dirty = 0;
      bl_last_tx = now;
      bl_next_toggle = now + bl_half_ns();
    }
    pthread_mutex_unlock(&state_lock);

    if (send) {
      m.seq = blink_seq++;
      uint8_t data[CAN_DLC_BLINKER];
      blinker_encode(&m, data);
      can_send_both(CAN_ID_BLINKER, data, CAN_DLC_BLINKER);
    }
  }
  return NULL;
}

////////////////// RX //////////////////
static void rx_reject(int bus, const struct can_frame *f, uint32_t id) {
#if RX_DEBUG
  dbg_frame("rx", bus, id, f->data, f->can_dlc, "DROPPED (invalid: unknown id, wrong dlc, or reserved bits set)");
#else
  (void) bus; (void) f; (void) id;
#endif
}

static void rx_frame(int bus, const struct can_frame *f) {
  int64_t now = now_ns();

  if (f->can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) return; // standard data frames only

  uint32_t id = f->can_id & CAN_SFF_MASK;
  int peer = -1; // where the HB is coming from
  int key = -1; // which dedup slot this frame belongs to
  uint8_t seq = 0;
  struct heartbeat hb = {0};
  struct force_feedback ff = {0};
  // decode also drops frames with the wrong length or a reserved bit set
  if (id == CAN_ID_HB_STEERING) {
    if (!hb_decode(f->data, f->can_dlc, &hb)) { rx_reject(bus, f, id); return; }
    peer = 0; key = K_HB2; seq = hb.seq;
  } else if (id == CAN_ID_HB_DRIVETRAIN) {
    if (!hb_decode(f->data, f->can_dlc, &hb)) { rx_reject(bus, f, id); return; }
    peer = 1; key = K_HB3; seq = hb.seq;
  } else if (id == CAN_ID_FORCE_FEEDBACK) {
    if (!force_decode(f->data, f->can_dlc, &ff)) { rx_reject(bus, f, id); return; }
    key = K_FORCE; seq = ff.seq;
  } else { rx_reject(bus, f, id); return; }

  // note when we heard it, before dedup, so a duplicate still shows the bus is alive
  pthread_mutex_lock(&state_lock);
  peer_frame[bus] = now;
  if (peer >= 0) hb_seen[bus][peer] = now;
  pthread_mutex_unlock(&state_lock);

  // dedup logic
  int dup = ((dd[key].t != INT64_MIN) && (dd[key].seq == seq) && ((now - dd[key].t) < DEDUP_MS * NS_PER_MS));
  if (!dup) {
    dd[key].seq = seq;
    dd[key].t = now;
  }

#if RX_DEBUG
  dbg_frame("rx", bus, id, f->data, f->can_dlc, dup ? "DROPPED (duplicate of a frame already accepted)" : "ACCEPTED");
#endif
  if (dup) return;

  if (id == CAN_ID_FORCE_FEEDBACK) {
    int8_t force = (int8_t) clamp(ff.force, -MAX_FORCE_VAL, MAX_FORCE_VAL);
    sendto(force_fd, &force, 1, MSG_DONTWAIT, (struct sockaddr *) &force_addr, sizeof(force_addr));
  }
}

// FORCE_RX thread: reads both buses (HB_STEERING, HB_DRIVETRAIN, FORCE_FEEDBACK)
static void *rx_thread(void *arg) {
  (void) arg;
  struct pollfd pfd[2] = {{.fd = can_fd[0], .events = POLLIN}, {.fd = can_fd[1], .events = POLLIN}};

  while (1) {
    poll(pfd, 2, -1); // sleep until a frame shows up on either bus
    for (int bus = 0; bus < 2; bus++) {
      if (pfd[bus].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        sleep_until(now_ns() + NS_PER_MS);
        continue;
      }
      struct can_frame f;
      // read everything waiting
      while (read(can_fd[bus], &f, sizeof(f)) == (ssize_t) sizeof(f)) { 
        rx_frame(bus, &f);
      }
    }
  }
  return NULL;
}

// monitor HBs
static int recent(int64_t t, int64_t now) {
  return (t != INT64_MIN) && ((now - t) < HB_MISS_MS * NS_PER_MS);
}

// run every 2ms to monitor the states 
static void *mon_thread(void *arg) {
  (void) arg;
  int64_t next = now_ns();
  int shown_state = -1;
  int shown_deg[2] = {0, 0};

  while (1) {
    next += MON_TICK_MS * NS_PER_MS;
    sleep_until(next);
    int64_t now = now_ns();

    pthread_mutex_lock(&state_lock);

    // check if zone is alive
    int alive_n2 = recent(hb_seen[0][0], now) || recent(hb_seen[1][0], now);
    int alive_n3 = recent(hb_seen[0][1], now) || recent(hb_seen[1][1], now);
    int all_alive = alive_n2 && alive_n3;

    // a bus is degraded if no peer frame at all came in on it recently. Clears on the next frame.
    int deg[2];
    for (int b = 0; b < 2; b++) deg[b] = !recent(peer_frame[b], now);
    health = (deg[0] ? 1 : 0) | (deg[1] ? 2 : 0);

    // enter: a peer went silent on both buses
    if (!in_err && !all_alive) enter_error();

    // exit: no local fault, both peers alive, valid UDP update since the error began
    if (in_err && !self_test && all_alive && udp_valid) {
      in_err = 0;
      bl_hazard(0);
    }

    int st = !in_err ? 0 : (self_test ? 2 : 1); // 0 = normal, 1 = error, 2 = self-test
    pthread_mutex_unlock(&state_lock);

    gpio_put(PIN_DEG_A, deg[0]);
    gpio_put(PIN_DEG_B, deg[1]);

    if (st != shown_state || deg[0] != shown_deg[0] || deg[1] != shown_deg[1]) { // print on change only
      printf("state=%d degA=%d degB=%d\n", st, deg[0], deg[1]);
      shown_state = st;
      shown_deg[0] = deg[0];
      shown_deg[1] = deg[1];
    }
  }
  return NULL;
}

int main(int argc, char **argv) {
  const char *ifname[2] = {"can0", "can1"};
  if (argc > 2) {
    ifname[0] = argv[1];
    ifname[1] = argv[2];
  }

  gpio_init();
  can_fd[0] = can_open(ifname[0]);
  can_fd[1] = can_open(ifname[1]);

  for (int b = 0; b < 2; b++) {
    hb_seen[b][0] = hb_seen[b][1] = INT64_MIN;
    peer_frame[b] = now_ns(); // start of a short grace period (used for the DEG logic later)
  }

  for (int k = 0; k < K_N; k++) dd[k].t = INT64_MIN;

  force_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (force_fd < 0) {
    perror("failed to create force socket");
    exit(EXIT_FAILURE);
  }
  force_addr.sin_family = AF_INET;
  force_addr.sin_port = htons(S_PORT);
  force_addr.sin_addr.s_addr = inet_addr(REMOTE_HOST);

  pthread_t hb_tid, blink_tid, rx_tid, mon_tid;
  pthread_create(&hb_tid, NULL, hb_thread, NULL);
  pthread_create(&blink_tid, NULL, blink_thread, NULL);
  pthread_create(&rx_tid, NULL, rx_thread, NULL);
  pthread_create(&mon_tid, NULL, mon_thread, NULL);

  // UDP socket for the wheel state from the proxy
  int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0) {
    perror("failed to create socket");
    exit(EXIT_FAILURE);
  }
  struct sockaddr_in servaddr = { 0 };
  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(R_PORT);
  servaddr.sin_addr.s_addr = inet_addr(LOCAL_HOST);
  if (bind(sockfd, (const struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  // The proxy sends: [4-byte packet counter][DIJOYSTATE2_t]
  uint8_t pkt[4 + sizeof(DIJOYSTATE2_t)];
  DIJOYSTATE2_t state;

  // remembered between updates (for the button edges)
  int prev_left = 0;
  int prev_right = 0;
  int btn_level = 0; // was the B button down at the last update
  int64_t btn_change = -DEBOUNCE_MS * NS_PER_MS; // when it last changed
  int64_t last_press = 0; // when the last single press happened

  while (1) {
    ssize_t n = recvfrom(sockfd, pkt, sizeof(pkt), 0, NULL, NULL);
    gpio_put(PIN_UDP_RX, 1); // rising edge = an update just arrived (scope trigger)
    if (n != (ssize_t) sizeof(pkt)) { // wrong size: not a valid update, ignore it
      gpio_put(PIN_UDP_RX, 0);
      continue;
    }
    memcpy(&state, pkt + 4, sizeof(state));

#if WHEEL_ON_MAC
    // Mac reports pedals as 0 (no press) .. 32767 (fully down); zone_1 expects
    // 32767 (no press) .. -32768 (fully down). Wheel range is already close enough.
    state.lY  = 32767 - 2 * (int32_t) state.lY;
    state.lRz = 32767 - 2 * (int32_t) state.lRz;
    state.lX = 32767 - 2 * (int32_t) state.lX;
    if (state.lX  < -32768) state.lX  = -32768;
    if (state.lY  < -32768) state.lY  = -32768;
    if (state.lRz < -32768) state.lRz = -32768;
#endif

    // axes go out raw, as in Lab 2: steering 0 = center, negative = left;
    // throttle negative = pressed; brake 32767 = not pressed, -32768 = fully pressed
    struct zone_ctrl c = {
      .steering = (int16_t) clamp(state.lX, -32768, 32767),
      .throttle = (int16_t) clamp(state.lY, -32768, 32767),
      .brake = (int16_t) clamp(state.lRz, -32768, 32767),
      .seq = ctrl_seq++,
    };
    if (state.rgbButtons[4]) c.buttons |= 1 << CTRL_BTN_RIGHT;
    if (state.rgbButtons[5]) c.buttons |= 1 << CTRL_BTN_LEFT;
    if (state.rgbButtons[0]) c.buttons |= 1 << CTRL_BTN_A;
    if (state.rgbButtons[1]) c.buttons |= 1 << CTRL_BTN_B;
    if (state.rgbButtons[2]) c.buttons |= 1 << CTRL_BTN_X;
    if (state.rgbButtons[3]) c.buttons |= 1 << CTRL_BTN_Y;

    uint8_t data[CAN_DLC_ZONE_CTRL];
    ctrl_encode(&c, data);
    gpio_put(PIN_CMD_TX, 1);           // CMD_TX is high while we write to the buses
    can_send_both(CAN_ID_ZONE_CTRL, data, CAN_DLC_ZONE_CTRL);
    gpio_put(PIN_CMD_TX, 0);

    // test button (B): single press fails this zone, double press clears it
    int b_down = state.rgbButtons[1] != 0;
    int left = state.rgbButtons[5] != 0;
    int right = state.rgbButtons[4] != 0;
    int angle = (c.steering + 32768) * 900 / 65535; // 0 = full left, 450 = center, 900 = full right
    int64_t now = now_ns();

    pthread_mutex_lock(&state_lock);
    udp_valid = 1; // set first: if this very update enters error below, it gets cleared again
    bl_steer(angle);
    if (left && !prev_left) bl_left(); // act on the press (the edge), not on holding
    if (right && !prev_right) bl_right();
    prev_left = left;
    prev_right = right;
    if (b_down != btn_level) { // B button changed
      int steady = (now - btn_change) >= DEBOUNCE_MS * NS_PER_MS;
      btn_level = b_down;
      btn_change = now;
      if (b_down && steady) { // a real press
        if (self_test && (now - last_press) <= DOUBLE_PRESS_MS * NS_PER_MS) {
          self_test = 0; // double press: cause cleared, HB restarts. HB_MON decides when we leave error.
        } else {
          self_test = 1; // single press: fail this zone now (HB goes silent, so the peers see us fail)
          enter_error();
          last_press = now;
        }
      }
    }
    pthread_mutex_unlock(&state_lock);

    gpio_put(PIN_UDP_RX, 0);
  }

  return 0;
}
