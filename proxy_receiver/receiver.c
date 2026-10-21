// Lab 3, N1 (Raspberry Pi). Transmit side: CTRL, HB_N1, BLINK, FAULT on can0 and can1, with test points.
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

/** Configure this **/
#define LOCAL_HOST "172.26.166.20" // IP of local interface
#define R_PORT 8000                // wheel state comes in here

#define REMOTE_HOST "172.26.76.97" // force goes back here (added in step 8)
#define S_PORT 8001

// set to 1 when using MAC
#define WHEEL_ON_MAC 0

// GPIO settings
#define PIN_UDP_RX 22
#define PIN_CMD_TX 27
#define PIN_TX_A   5
#define PIN_TX_B   6

#define ID_CTRL  0x010
#define ID_FAULT 0x020
#define ID_HB_N1 0x100
#define ID_HB_N2 0x101
#define ID_HB_N3 0x102
#define ID_BLINK 0x200
#define ID_FORCE 0x300

#define RX_DEBUG 1  // SET TO PRINT ALL FRAMES

#define HB_PERIOD_MS      20
#define BLINK_TICK_MS     2     // how often the blink thread checks if it has to send
#define BLINK_HALF_MS     500   // 1 Hz: 500 ms on, 500 ms off
#define BLINK_MIN_GAP_MS  20    // never send BLINK closer together than this
#define DEBOUNCE_MS       20    // test button (same as Lab 2)
#define DOUBLE_PRESS_MS   500
#define LEFT_THRESHOLD    299   // wheel angle 0..900, from Lab 2 blinker.c
#define RIGHT_THRESHOLD   601

enum { B_OFF = 0, B_LEFT, B_RIGHT, B_HAZARD };       // BLINK state byte
enum { Z_NORMAL = 0, Z_LINK_LOST = 1, Z_SELF_TEST = 2 }; // zone_state values used so far
#define FAULT_TEST_BTN 1                             // FAULT reason

// button byte bits 
#define BTN_RIGHT 0x01
#define BTN_X     0x04
#define BTN_B     0x08
#define BTN_A     0x10
#define BTN_Y     0x20
#define BTN_LEFT  0x80

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

  int pins[] = {PIN_UDP_RX, PIN_CMD_TX, PIN_TX_A, PIN_TX_B}; // GPIO pins init
  for (int i = 0; i < 4; i++) {
    int p = pins[i];
    // each pin has a 3-bit mode field, 10 pins per register. Mode 1 = output.
    gpio[p / 10] = (gpio[p / 10] & ~(7 << ((p % 10) * 3))) | (1 << ((p % 10) * 3));
    gpio[10] = 1 << p; // register 10 clears pins (start low)
  }
}

static void gpio_put(int pin, int value) {
  if (gpio) gpio[value ? 7 : 10] = 1 << pin; // register 7 sets pins, register 10 clears them
}

// CRC Check
static uint8_t crc8(const uint8_t *data, int len) {
  uint8_t crc = 0;
  for (int i = 0; i < len; i++) {
    crc ^= data[i];
    // crc math
    for (int bit = 0; bit < 8; bit++) { 
      if (crc & 0x80) crc = (crc << 1) ^ 0x07;
      else crc = crc << 1;
    }
  }
  return crc;
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
  // everything else on the bus never wakes us up
  uint32_t mask = CAN_SFF_MASK | CAN_EFF_FLAG | CAN_RTR_FLAG;
  struct can_filter filters[] = {
    {ID_FAULT, mask}, {ID_HB_N2, mask}, {ID_HB_N3, mask}, {ID_FORCE, mask},
  };
  setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, filters, sizeof(filters));

  // Non-blocking: if a bus is dead and its queue is full, write() fails right away
  // instead of freezing us. A dead bus must not stop the other bus.
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

static int can_fd[2]; // [0] = bus A, [1] = bus B
static const int tx_pin[2] = {PIN_TX_A, PIN_TX_B};

// seq ctrs -- wraps after 15
static uint8_t ctrl_seq = 0, hb_seq = 0, blink_seq = 0, fault_seq = 0;

// main thread: CTRL & FAULT
// thread 1: HB 
// thread 2: BLINK
static pthread_mutex_t tx_lock = PTHREAD_MUTEX_INITIALIZER;

// send frame on both buses. Caller fills bytes 0..6; byte 7 (CRC over 0..6) is added here,
// so every message we send carries a CRC.
static void can_send_both(uint32_t id, const uint8_t data[8]) {
  struct can_frame f = {0};
  f.can_id = id;
  f.can_dlc = 8;
  memcpy(f.data, data, 7);
  f.data[7] = crc8(f.data, 7);

  pthread_mutex_lock(&tx_lock);
  for (int bus = 0; bus < 2; bus++) {
    gpio_put(tx_pin[bus], 1);
    write(can_fd[bus], &f, sizeof(f)); // if it fails (bus dead / queue full) we just move on
    gpio_put(tx_pin[bus], 0);
  }
  pthread_mutex_unlock(&tx_lock);
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

// STATE TRACKING
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

// real power-up state should be Z_LINK_LOST w hazards
static uint8_t zone_state = Z_NORMAL; // Z_NORMAL, Z_LINK_LOST, Z_SELF_TEST
static uint8_t health = 0; // bit0 = bus A degraded, bit1 = bus B 
static int self_test = 0; // set while the wheel test button has failed this zone

// BLINKERS
static int bl_state = B_OFF; // B_OFF, B_LEFT, B_RIGHT, B_HAZARD,
static int bl_phase = 0; // 1 = lamps on
static int bl_dirty = 1; // 1 = state changed, a BLINK must go out (starts 1: announce OFF)
static int bl_passed = 0; // wheel went past the threshold; turn cancels when it comes back
static int64_t bl_next_toggle; // when the 500 ms phase flip is due
static int64_t bl_last_tx = INT64_MIN;

// RX TRACKING 
#define NEVER INT64_MIN
static int64_t hb_seen[2][2]; // [bus][peer], peer 0 = N2, 1 = N3. when we last heard its HB
static int64_t peer_frame[2]; // [bus] when we last heard ANY peer frame on that bus

static void bl_set(int s) {
  if (bl_state == s) return;
  bl_state = s;
  bl_passed = 0;
  bl_phase = (s == B_OFF) ? 0 : 1; // a change restarts the pattern with lamps on
  bl_dirty = 1;                    // and the blink thread sends it right away
}

static void bl_left(void)  { if (bl_state != B_HAZARD) bl_set(B_LEFT); }  // hazards win
static void bl_right(void) { if (bl_state != B_HAZARD) bl_set(B_RIGHT); }

static void bl_hazard(int on) {
  if (on) bl_set(B_HAZARD);
  else if (bl_state == B_HAZARD) bl_set(B_OFF);
}

// Cancel the turn signal once the wheel goes past the threshold and comes back.
static void bl_steer(int angle) {
  if (bl_state == B_LEFT) {
    if (angle < LEFT_THRESHOLD) bl_passed = 1;
    else if (bl_passed) bl_set(B_OFF);
  } else if (bl_state == B_RIGHT) {
    if (angle > RIGHT_THRESHOLD) bl_passed = 1;
    else if (bl_passed) bl_set(B_OFF);
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
    uint8_t zs = zone_state, h = health;
    pthread_mutex_unlock(&state_lock);
    if (silent) continue;

    uint8_t data[8] = {0};
    data[0] = hb_seq++ & 0x0F;
    data[1] = zs;
    data[2] = h;
    can_send_both(ID_HB_N1, data);
  }
  return NULL;
}

// ---------- BLINK_TX thread: we are the blinker master ----------
// Wakes every 2 ms. Sends BLINK when the state just changed, or when 500 ms have passed
// (the STM zones flip their lamps when they receive it, so they stay in sync with each other).
// BTLINK TX thread, wakes every 2ms
static void *blink_thread(void *arg) {
  (void) arg;
  int64_t next = now_ns();

  pthread_mutex_lock(&state_lock);
  bl_next_toggle = now_ns() + BLINK_HALF_MS * NS_PER_MS;
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
      if (bl_state != B_OFF) bl_phase ^= 1;        // 500 ms passed: flip lamps on/off
      send = 1;
    }
    uint8_t data[8] = {0};
    data[1] = bl_state;
    data[2] = bl_phase;
    if (send) {
      bl_dirty = 0;
      bl_last_tx = now;
      bl_next_toggle = now + BLINK_HALF_MS * NS_PER_MS;
    }
    pthread_mutex_unlock(&state_lock);

    if (send) {
      data[0] = blink_seq++ & 0x0F;
      can_send_both(ID_BLINK, data);
    }
  }
  return NULL;
}

////////////////// RX //////////////////
static void rx_frame(int bus, const struct can_frame *f) {
  int64_t now = now_ns();

  if (f->can_dlc != 8) return;
  if (f->can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) return; // standard data frames only
  if (crc8(f->data, 7) != f->data[7]) return;            

  uint32_t id = f->can_id & CAN_SFF_MASK;
  int peer = -1;
  if (id == ID_HB_N2) peer = 0;
  else if (id == ID_HB_N3) peer = 1;
  else if (id == ID_FAULT) {
    if (f->data[1] == 1) return; // FAULT from zone 1 is us (a relay echoed it back)
  } else if (id != ID_FORCE) return;

  // note when we heard it, before dedup, so a duplicate still shows the bus is alive
  pthread_mutex_lock(&state_lock);
  peer_frame[bus] = now;
  if (peer >= 0) hb_seen[bus][peer] = now;
  pthread_mutex_unlock(&state_lock);

#if RX_DEBUG
  printf("rx bus%c id=0x%03X seq=%u data=%02X %02X %02X %02X %02X %02X %02X\n", 'A' + bus,
         (unsigned) id, f->data[0] & 0x0F, f->data[1], f->data[2], f->data[3], f->data[4],
         f->data[5], f->data[6], f->data[7]);
#endif
}

// FORCE_RX thread: reads both buses (HB, FAULT, FORCE)
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


int main(int argc, char **argv) {
  const char *ifname[2] = {"can0", "can1"};
  if (argc > 2) {
    ifname[0] = argv[1];
    ifname[1] = argv[2];
  }

  // test CRC if needed
  // if (crc8((const uint8_t *) "123456789", 9) != 0xF4) {
  //   fprintf(stderr, "crc8 self-check failed\n");
  //   return EXIT_FAILURE;
  // }

  gpio_init();
  can_fd[0] = can_open(ifname[0]);
  can_fd[1] = can_open(ifname[1]);

  for (int b = 0; b < 2; b++) {
    hb_seen[b][0] = hb_seen[b][1] = NEVER;
    peer_frame[b] = now_ns(); // start of a short grace period (used for the DEG logic later)
  }

  pthread_t hb_tid, blink_tid, rx_tid;
  pthread_create(&hb_tid, NULL, hb_thread, NULL);
  pthread_create(&blink_tid, NULL, blink_thread, NULL);
  pthread_create(&rx_tid, NULL, rx_thread, NULL);

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

  // remembered between updates (for edge detection on the buttons)
  int prev_left = 0, 
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

    // ---- step 3: turn wheel state into the CTRL fields ----
    // steering: the raw 16-bit value, 0 = center, negative = left
    int steer = clamp(state.lX, -32768, 32767);
    // throttle: wheel says negative = pressed. Flip it, then squeeze 0..32767 into 0..255.
    int throttle = clamp(-state.lY, 0, 32767) * 255 / 32767;
    // brake: wheel says 32767 = not pressed, -32768 = fully pressed. Flip into 0..255.
    int brake = clamp((32767 - state.lRz) * 255 / 65535, 0, 255);

    uint8_t buttons = 0;
    if (state.rgbButtons[4]) buttons |= BTN_RIGHT;
    if (state.rgbButtons[5]) buttons |= BTN_LEFT;
    if (state.rgbButtons[0]) buttons |= BTN_A;
    if (state.rgbButtons[1]) buttons |= BTN_B;
    if (state.rgbButtons[2]) buttons |= BTN_X;
    if (state.rgbButtons[3]) buttons |= BTN_Y;

    // ---- step 4: build and send CTRL ----
    uint8_t data[8];
    data[0] = ctrl_seq++ & 0x0F;       // sequence number (top 4 bits stay 0)
    data[1] = (steer >> 8) & 0xFF;     // steer high byte
    data[2] = steer & 0xFF;            // steer low byte
    data[3] = throttle;
    data[4] = brake;
    data[5] = buttons;
    data[6] = 0;
    data[7] = 0;                       // CRC is filled in by can_send_both

    gpio_put(PIN_CMD_TX, 1);           // CMD_TX is high while we write to the buses
    can_send_both(ID_CTRL, data);
    gpio_put(PIN_CMD_TX, 0);

    // ---- blinker and test button. After the CTRL send, so they never delay it. ----
    int angle = (steer + 32768) * 900 / 65535;   // 0 = full left, 450 = center, 900 = full right
    int left = state.rgbButtons[5] != 0
    int right = state.rgbButtons[4] != 0;
    int b_down = state.rgbButtons[1] != 0;
    int64_t now = now_ns();
    int send_fault = 0;

    pthread_mutex_lock(&state_lock);
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
          self_test = 0; // double press: back to normal
          zone_state = Z_NORMAL; // TEMP, the real exit rule comes in step 10
          bl_hazard(0);
        } else {
          self_test = 1; // single press: fail this zone now
          zone_state = Z_SELF_TEST;
          bl_hazard(1);
          last_press = now;
          send_fault = 1;
        }
      }
    }
    pthread_mutex_unlock(&state_lock);

    if (send_fault) {
      uint8_t f[8] = {0};
      f[0] = fault_seq++ & 0x0F;
      f[1] = 1; // zone that faulted: N1 (the Pi)
      f[2] = FAULT_TEST_BTN; // reason for failure
      can_send_both(ID_FAULT, f);
    }

    gpio_put(PIN_UDP_RX, 0);
  }

  return 0;
}
