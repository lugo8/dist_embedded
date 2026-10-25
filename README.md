# 18-449/649 Lab 3: dual-redundant CAN (Team 6)

```
common/can_msg.h     message catalog, CTRL encode/decode, dedup (shared by Pi and STM32)
pi/cockpit.c         N1 cockpit zone (Raspberry Pi 5)
pi/setup.sh          bring up can0/can1 (config.txt lines are in the comments)
pi/check_bus.py      checkoff step 2: records both buses, checks every ID's period and seq counter
stm32/               one Zephyr app for both STM32s; -DZONE picks N2 (front) or N3 (rear)
proxy/               Windows wheel proxy (Mac: mac_proxy.py from the course repo)
west-manifest/       pins Zephyr v4.4.0
```

## Build and run

**STM32** (from the west workspace, Zephyr v4.4.0):
```
west build -b nucleo_f401re -d build-front stm32 -- -DZONE=front && west flash -d build-front
west build -b nucleo_f401re -d build-rear  stm32 -- -DZONE=rear  && west flash -d build-rear
```

**Pi:**
```
./pi/setup.sh
sudo apt install liblgpio-dev     # once
gcc -O2 -Wall -o pi/cockpit pi/cockpit.c -lpthread -llgpio
sudo ./pi/cockpit
```

**Laptop** (Ethernet to the Pi; laptop 192.168.50.1/24, Pi 192.168.50.2/24, no gateway):
```
cd proxy && python proxy_gui.py              # Windows
python3 mac_proxy.py --remote 192.168.50.2 --interval-ms 20 ...   # Mac, and set WHEEL_ON_MAC 1 in cockpit.c
```

## Message catalog (handout template)

| ID | Name | From | Period | Payload (byte 0 = seq low nibble + flags) |
|---|---|---|---|---|
| 0x010 | CTRL | N1 | every UDP update, 20 ms | b1-2 steer int16 LE (deg), b3 throttle %, b4 brake %, b5 buttons, b6 CRC-8 of b1..b5 |
| 0x020 | FAULT | any | on error entry | b1 failing zone, b2 reason (1 HB lost, 2 bad CTRL, 3 test button), b3 sender |
| 0x100/101/102 | HB_N1/N2/N3 | each zone | 20 ms | b1 state (0 normal, 1 error), b2 bit0 bus A degraded, bit1 bus B degraded |
| 0x200 | BLINK | N1 | on change + every 500 ms | b1 off/L/R/hazard, b2 phase (1 = lamps on) |
| 0x300 | FORCE | N2 | 50 ms | b1 int8 dummy counter |

All DLC 8, sent on both buses back to back, seq bumped once per message.

## Team-defined values

| Value | Range |
|---|---|
| steer | -450..450 deg |
| throttle, brake | 0..100 % |
| buttons | bit0 left, bit1 right, bit2 test; bits 3-7 must be 0 |
| CTRL CRC-8 | poly 0x07 over bytes 1..5 |

Anything outside these puts the receiving zone in error for 1 s and sends FAULT.
Wheel button Y on the Pi sends one out-of-range CTRL (bad-input demo).

## Design decisions

- **Dedup:** one entry per ID holding the last accepted seq and the buses it arrived on. The same seq from the other bus is the duplicate copy; the same seq again on the same bus is a new frame (counter wrapped).
- **Liveness:** a zone is alive if its heartbeat arrived on either bus in the last 65 ms (3 missed + margin). A bus is degraded (DEG LED) if no frame from any other zone arrived on it in 45 ms, so DEG comes on within 60 ms of a pull.
- **Error state:** power up in error. Leave only with both other zones' heartbeats seen and a valid CTRL received since the error began. Enter on test button (single press; double press clears), out-of-range CTRL, a zone missing on both buses, or no CTRL for 100 ms. In error N3 brakes and both STM32s flash hazards (following the cockpit's BLINK phase when it sends hazard, otherwise locally).
- **Test button:** Nucleo user button B1 on each STM32, wheel button B on the Pi. A zone in self-test stops its heartbeat, which is how the others detect it.
- **Relay:** only N2 relays, forward-on-detect. A frame from zone Z arriving on one bus is copied to the other when Z's heartbeat has been missing there for 30 ms. That is one missed heartbeat, well under the 65 ms failure threshold, so relayed heartbeats arrive before anyone declares Z failed. Relayed frames keep their ID and seq; receivers drop them as duplicates if the direct copy also arrived.
- **Priorities (Zephyr, lower = higher):** MCP2515 driver 2, RELAY 3, CAN_RX dispatch 4, DRIVE/STEER/BLINK 5, TEST_BTN/PID 6, HB_TX/HB_MON 7, FORCE_TX 8.
- **Pi:** runs SCHED_FIFO priority 40 (below the kernel CAN irq threads) with locked memory (needs sudo). Bus A/B are picked by chip select (CE0/CE1), not by can0/can1 name. A bus the Pi hears nobody on is only probed every 100 ms.
- **Watchdog:** IWDG on each STM32, 100 ms, fed by HB_MON.

## Test points

| Row | N1 (Pi 5, BCM / header pin) | N2 front | N3 rear |
|---|---|---|---|
| 3 | UDP_RX GPIO22 / 15 | CAN_RX_A A0 PA0 | CAN_RX_A A0 PA0 |
| 4 | CMD_TX GPIO27 / 13 | CAN_RX_B A1 PA1 | CAN_RX_B A1 PA1 |
| 5 | CAN_TX_A GPIO5 / 29 | CAN_TX_A A2 PA4 | CAN_TX_A A2 PA4 |
| 6 | CAN_TX_B GPIO6 / 31 | CAN_TX_B A3 PB0 | CAN_TX_B A3 PB0 |
| 7 | DEG_A GPIO13 / 33 | DEG_A D7 PA8 | DEG_A D7 PA8 |
| 8 | DEG_B GPIO16 / 36 | DEG_B D8 PA9 | DEG_B D8 PA9 |
| 9 | - | SRV D3 PB3 | PWM_SET A4 PC1 |
| 10 | - | FL D5 PB4 | PWM_OUT D3 PB3 |
| 11 | - | FR D6 PB10 | DIR_A A5 PC0 |
| 12 | - | RELAY_TX A4 PC1 | RL D5 PB4 |
| 13 | - | ERR A5 PC0 | RR D6 PB10 |

CAN modules: STM32 SPI1 PA5/PA6/PA7, CS A PB6 / B PC7, INT A PA10 / B PB5.
Pi SPI0, CS CE0 / CE1, INT GPIO25 / GPIO24 (if config.txt says interrupt=26 for can0, that wire goes to GPIO26). Test button: B1 (PC13) on each Nucleo.
CAN_RX, CAN_TX and RELAY_TX are high for the duration of the work, so the pulse width is the WCET sample.
