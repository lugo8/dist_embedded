# 18-449/649 Lab 5: Raft and time synchronization over dual-redundant CAN (Team 6)

```
raft.conf            Raft configuration (Part 2), identical on every node; both builds read it
common/can_msg.h     message catalog, encode/decode, dedup (shared by Pi and STM32)
common/raft.[ch]     Raft agent + client + network state machine (Parts 1-5), no OS
common/route.h       relay choice from the network graph (Part 5)
common/timesync.[ch] clock sync + drift (Part 6), integer math, no OS
pi/cockpit.c         N1 cockpit zone (Raspberry Pi 5)
pi/raft_node.c       N1's Raft/time-sync/PPS threads and its log file
pi/setup.sh          bring up can0/can1 (config.txt lines are in the comments)
pi/check_bus.py      checkoff step 2: periods, seq counters, REF cycle/time, empty APPEND, no REQ_VOTE
stm32/               one Zephyr app for both STM32s; -DZONE picks N2 (front) or N3 (rear)
stm32/src/raft_agent.c  Raft agent thread, REF_TX, time sync, LEADER/SYNC LEDs, printouts
stm32/src/pps.c      PPS output (Part 6.4)
tests/               host simulations: 3 nodes x 2 buses through every checkoff step; time sync
tools/               gen_raft_conf.py (raft.conf -> raft_conf.h), find_seed.py (LEADER=random)
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
sudo apt install liblgpio-dev     # once
sh pi/run.sh                      # every run: CAN up, build, start cockpit
```
`run.sh` = `setup.sh` + build (`raft.conf` -> header, then gcc) + `sudo ./pi/cockpit`, run from the
repo root, so `raft_cockpit.log` always lands in the same place. `make -C pi -f cockpit.mk` builds
without running.

**Configuration:** edit `raft.conf`, then rebuild the Pi (`make -C pi -f cockpit.mk`) and rebuild and reflash
both STM32s. Checkoff step 1 uses `default_leader = cockpit`; step 8 uses `default_leader = random`
with `seed = 48` (an STM32 wins) and then `seed = 23` (the Pi wins). `python3 tools/find_seed.py`
lists more. With `random`, start the proxy before rebooting: nobody campaigns until every node
has been heard, and the Pi only talks once its wheel stream is up.

**Tests (on a laptop):** `make -C tests -f sim.mk` runs every checkoff scenario in a 3-node, 2-bus
simulation (jumpers, relaying, power cycles, the Pi's log file) and a time-sync simulation
with three drifting crystals.

**Laptop** (Ethernet to the Pi; laptop 192.168.50.1/24, Pi 192.168.50.2/24, no gateway):
```
cd proxy && python proxy_gui.py              # Windows
python3 mac_proxy.py --remote 192.168.50.2 --interval-ms 20 ...   # Mac, and set WHEEL_ON_MAC 1 in cockpit.c
```

## Message catalog (handout template)

| ID | Name | From | Period | Payload (byte 0 = seq low nibble + flags) |
|---|---|---|---|---|
| 0x010 | CTRL | N1 | every UDP update, 20 ms | b1-2 steer int16 LE (deg), b3 throttle %, b4 brake %, b5 buttons, b6 CRC-8 of b1..b5 and b7, b7 bits 0-1 turn state (off/L/R/hazard; Lab 5, replaces BLINK), bits 2-7 turn start slot (50 ms grid point of global time, mod 64) |
| 0x020 | FAULT | any | on error entry | b1 failing zone, b2 reason (1 HB lost, 2 bad CTRL, 3 test button), b3 sender |
| 0x030/031/032 | REF_Nx | Raft leader | 20 ms (`heartbeat_ms`) | b1 term, b2 cycle, b3-6 master time µs bits 0-31 LE, b7 bit0 failure mode + bits1-7 master time bits 32-38 (39 bits, 6.4 days). Flag 0x20 = time is end of previous REF on this bus |
| 0x040/041/042 | VOTE_REQ_Nx | candidate | on election | b1 term, b2-3 last log index, b4 last log term |
| 0x048/049/04A | VOTE_RSP_Nx | any | on VOTE_REQ | b1 term, b2 candidate, b3 granted |
| 0x050/051/052 | APPEND_Nx | Raft leader | per entry (≤ 1 / 5 ms), empty every 100 ms | b1 term, b2-3 index, b4 prev term, b5 entry term, b6 event (0 = empty), b7 commit lag (index − commit, ≤ 255) |
| 0x058/059/05A | APPEND_ACK_Nx | follower | per APPEND | b1 term, b2 success, b3-4 match index |
| 0x060/061/062 | CLIENT_REQ_Nx | Raft client | on a detected change | b1 term, b2 node addressed, b3 request id, b4 event |
| 0x068/069/06A | CLIENT_RSP_Nx | Raft agent | on commit / redirect | b1 term, b2 client node, b3 request id, b4 status (0 ok, 1 redirect, 2 busy), b5 leader, b6-7 log index |
| 0x100/101/102 | HB_N1/N2/N3 | each zone | 20 ms | b1 state (0 normal, 1 error), b2 bit0 bus A degraded, bit1 bus B degraded; Lab 5: b3 sync bits |
| ~~0x200~~ | ~~BLINK~~ | N1 | Lab 3 only, being removed | state moved to CTRL b7; lamp phase comes from global time |
| 0x300 | FORCE | N2 | 50 ms | b1 int8 dummy counter |

All DLC 8, sent on both buses back to back, seq bumped once per message.
Raft IDs are base + (sender − 1), so no two nodes ever send the same ID (arbitration needs one sender per ID) and dedup stays per ID.
Terms go on the wire as their low 8 bits; the receiver extends them against its own term (`term_unwire`).
Log events (1 byte): 0x01/0x02 COCKPIT_OK/ERROR, 0x1X/0x2X NODE_X_UP/DOWN, 0x3L/0x4L CANLINK up/down with L = (X−1)·2 + bus, 0x80+ team events.
Worst-case load per bus: 8.1 % steady, 24.6 % during log catch-up, 34.6 % with a relay active (see the header comment in `can_msg.h`).

## Lab 5 design decisions

- **Roles:** one Raft agent per node. The Pi runs it as threads inside `cockpit`; each STM32 runs it in the RAFT thread (prio 7, 2 ms tick). The CAN dispatch thread only queues frames for it, so nothing on the brake path waits for Raft.
- **Heartbeat = REF:** the leader's 20 ms REF is the Raft heartbeat (it resets election timers) and the time reference. The empty APPEND every 100 ms carries the commit index and the log-consistency check; the leader also sends one right after every commit.
- **Pre-defined leader:** at boot a node listens for 149 ms (one maximum election timeout + 50 ms). It joins a running cluster if it hears REF or APPEND. If not, it's a cold start: the log is cleared and everyone is in term 1 with `default_leader` leading. Followers never campaign until they have heard that leader once, so the STM32s (up long before the Pi) don't elect themselves while the Pi boots.
- **`random`:** no node campaigns until every node has been heard since boot. Then every node waits the same listen time and starts a normal election, with timeouts drawn from a PRNG seeded by `seed` and the node ID.
- **No disruption from an isolated node:** a node only starts an election while it hears at least one other node (in the last 50 ms). A leader that hears nobody for 250 ms (over twice the empty-APPEND period, since a follower with its heartbeat off is heard only through its ACKs) steps down without a new term. Nodes ignore vote requests while they hear a live leader. So isolating a follower never changes the term, and isolating the leader changes it exactly once (checkoff 10).
- **Votes after a reboot:** a node in its boot listen window does not vote (it may have voted in this term before it reset).
- **Log:** one byte per event (30 KB for 30000 entries), terms kept as runs, index implicit. A new leader logs `LEADER_X` (the Raft no-op) so it can commit entries left over from the old term.
- **Client:** each node's client compares what it sees with the committed graph and reports differences, one at a time, retrying every 50 ms. Followers redirect; the leader drops reports that change nothing, so each fault gives one MEMBER entry. A node reports its own links (link y down = no other node heard directly on y for 250 ms) and node liveness (nothing at all from x for 300 ms). The leader confirms node reports against its own view before logging them.
- **Cockpit failure mode:** `NODE_1_DOWN` is logged with `COCKPIT_ERROR`, and `NODE_1_UP` with `COCKPIT_OK`. The Pi stopping its heartbeat while still running (proxy off, test button) is the Lab 3 error state at 2 Hz, not failure mode: it is still the leader in step 1. Failure mode is 4 Hz hazards. It starts on the 250 ms boundary after the REF whose mode bit first carries it, so every node switches on the same edge. Losing an STM32 is the Lab 3 error state (2 Hz), with no failure mode (checkoff 8).
- **Routing (Part 5):** Lab 3's rule stays, because it hands over within one missed heartbeat (30 ms, inside the 65 ms heartbeat timeout): N2 relays; N3 only when it can't hear N2's heartbeat on both buses; and new, the Pi as the last resort when neither STM32 is on both buses (checkoff 9). The Raft network graph removes a node from that list while it has the node off a bus, on every node alike (`route.h`). Exactly one node relays. A first version picked the lowest-numbered healthy node (the Pi) from heartbeat DEG bits; its handover took 50–70 ms, so zones dropped into the error state at every link pull.
- **Events (EVENT entries):** test-button press (the node pressed), blinker on/off (logged by N2 from CTRL b7), bad CTRL (each STM32 that got it).
- **Time sync (Part 6):**
  - REF is sent per bus in follow-up form: each copy carries the time the previous REF finished on that bus at the master (STM32: MCP2515 TX-done callback; Pi: the driver's echo of our own frame). Followers pair it with their receive time of that frame on that bus (STM32: stamped in the driver callback; Pi: kernel SocketCAN timestamp).
  - Drift is measured over ≥ 1 s and averaged. The handout's per-heartbeat ratio is printed alongside.
  - Phase errors are slewed out over 100 ms (at most 1000 ppm), never stepped, so a leader handover moves global time by microseconds. Only the first sync, or an error over 2 ms, steps.
  - Followers set the Zephyr system clock with `sys_clock_settime(SYS_CLOCK_REALTIME)` (what POSIX `clock_settime()` calls).
  - A new leader keeps running on its synced clock.
  - Relayed REFs are used only when no direct REF has arrived for 60 ms, with 300 µs added for the relay.
- **Blinkers:** lamps change only on a 50 ms grid of global time. Hazards count from global time 0 (2 Hz error, 4 Hz failure). A turn signal counts 1 Hz from the grid point the Pi picks and sends in CTRL b7 (10–60 ms after the button, R2.4), so N2 and N3 start on the same point even when one of them hears CTRL through the relay a millisecond later.
- **Pi log file:** written by a low-priority thread within 100 ms of a change (an SD card write can take tens of ms; it never blocks CAN receive). A Pi power cut can lose the last 100 ms of state.

## Team-defined values

| Value | Range |
|---|---|
| steer | -450..450 deg |
| throttle, brake | 0..100 % |
| buttons | bit0 left, bit1 right, bit2 test; bits 3-7 must be 0 |
| CTRL CRC-8 | poly 0x07 over bytes 1..5 and 7 |
| CTRL b7 | bits 0-1: 0 off, 1 left, 2 right, 3 hazard; bits 2-7: turn start slot = (global µs / 50000) mod 64, picked by the Pi 10–60 ms ahead |
| app events | 0x80 \| kind << 3 \| node; kind 1 test button, 2 left on, 3 right on, 4 hazard on, 5 blinker off, 6 bad CTRL |
| LEADER_X | 0x05-0x07 |

Anything outside these puts the receiving zone in error for 1 s and sends FAULT.
Wheel button Y on the Pi sends one out-of-range CTRL (bad-input demo).

## Lab 3 design decisions

- **Dedup:** one entry per ID holding the last accepted seq and the buses it arrived on. The same seq from the other bus is the duplicate copy; the same seq again on the same bus is a new frame (counter wrapped).
- **Liveness:** a zone is alive if its heartbeat arrived on either bus in the last 65 ms (3 missed + margin). A bus is degraded (DEG LED) if no frame from any other zone arrived on it in 45 ms, so DEG comes on within 60 ms of a pull.
- **Error state:** power up in error. Leave only with both other zones' heartbeats seen and a valid CTRL received since the error began. Enter on test button (single press; double press clears), out-of-range CTRL, a zone missing on both buses, or no CTRL for 250 ms. The Pi also fails itself (stops its heartbeat, sends FAULT reason 4) if no wheel update arrives for 100 ms, and recovers when the stream resumes. In error N3 brakes and both STM32s flash hazards (Lab 5: on global time, so in step with or without the cockpit).
- **Test button:** Nucleo user button B1 on each STM32, wheel button B on the Pi. A zone in self-test stops its heartbeat, which is how the others detect it.
- **Relay (Lab 3; Lab 5 changes who relays, see above):** both STM32s relay, forward-on-detect: a frame from zone Z arriving on one bus is copied to the other when Z's own heartbeat has been missing there for 30 ms (one missed heartbeat, well under the 65 ms failure threshold). N2 relays in checkoff cases 1-2, N3 in cases 3-4; the Pi never relays. N3 relays only when it can't hear N2's heartbeat on one of the buses, so two boards never relay onto the same bus (if both did with N1 pulled, nobody would be left to ACK the copies and the bus would jam). A forwarded copy sets FLAG_RELAYED (byte 0, 0x10) and is never forwarded again, and the relay decision counts only heartbeats heard directly, so two relays can't loop or switch each other off. Receivers drop a relayed copy as a duplicate if the direct copy also arrived.
- **Priorities (Zephyr, lower = higher):** MCP2515 driver 2, RELAY/PPS 3, CAN_RX dispatch 4, DRIVE/STEER/BLINK 5, TEST_BTN/PID/REF_TX 6, HB_TX/HB_MON/RAFT 7, FORCE_TX 8, PRINT 9.
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
| 14 | PPS GPIO17 / 11 | PPS D15 PB8 | PPS D15 PB8 |
| 15 | REF_TX GPIO20 / 38 | REF_RX D14 PB9 | REF_RX D14 PB9 |
| 16 | LEADER LED GPIO23 / 16 | LEADER LED CN10-2 PC8 | LEADER LED CN10-2 PC8 |
| 17 | SYNC LED GPIO12 / 32 | SYNC LED CN10-4 PC6 | SYNC LED CN10-4 PC6 |

CAN modules: STM32 SPI1 PA5/PA6/PA7, CS A PB6 / B PC7, INT A PA10 / B PB5.
Pi SPI0, CS CE0 / CE1, INT GPIO25 / GPIO24 (if config.txt says interrupt=26 for can0, that wire goes to GPIO26). Test button: B1 (PC13) on each Nucleo.
CAN_RX, CAN_TX and RELAY_TX are high for the duration of the work, so the pulse width is the WCET sample.
REF_RX/REF_TX toggle on every REF received/sent. PC6/PC8 are on the Nucleo's morpho header (CN10): wire them to LEDs if the PCB doesn't break them out.
