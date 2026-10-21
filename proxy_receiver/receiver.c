// Lab 3, N1 (Raspberry Pi). Steps 2-5: UDP in -> CTRL frame out on can0 and can1, with test points.
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

#define ID_CTRL 0x010

// button byte bits 
#define BTN_RIGHT 0x01
#define BTN_X     0x04
#define BTN_B     0x08
#define BTN_A     0x10
#define BTN_Y     0x20
#define BTN_LEFT  0x80

// ---------- test points ----------
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

  // Non-blocking: if a bus is dead and its queue is full, write() fails right away
  // instead of freezing us. A dead bus must not stop the other bus.
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  return fd;
}

static int can_fd[2]; // [0] = bus A, [1] = bus B
static const int tx_pin[2] = {PIN_TX_A, PIN_TX_B};

// wraps after 15
static uint8_t ctrl_seq = 0;

// send frame on both buses. Caller fills bytes 0..6; byte 7 (CRC over 0..6) is added here,
// so every message we send carries a CRC.
static void can_send_both(uint32_t id, const uint8_t data[8]) {
  struct can_frame f = {0};
  f.can_id = id;
  f.can_dlc = 8;
  memcpy(f.data, data, 7);
  f.data[7] = crc8(f.data, 7);

  for (int bus = 0; bus < 2; bus++) {
    gpio_put(tx_pin[bus], 1);
    write(can_fd[bus], &f, sizeof(f)); // if it fails (bus dead / queue full) we just move on
    gpio_put(tx_pin[bus], 0);
  }
}

static int clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
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

    gpio_put(PIN_UDP_RX, 0);
  }

  return 0;
}
