# dist_embedded

## Split Zone 1 CAN applications

`stm32_front/` and `stm32_back/` are separate Zephyr applications for the
Nucleo-F401RE. Both use the two MCP2515 controllers and 500 kbit/s CAN buses
configured in `stm_can`.

- The front application handles steering-servo PWM, front indicators, the
  error indicator, CAN reception/relay, and status transmission.
- The back application retains Zone 1's dual-motor PID/brake-hold control,
  rear indicators, CAN reception/relay, and status transmission. Its
  quadrature encoders use PC0-PC3; motor PWM uses PB0/PB1; motor direction
  uses PA13 and PB7. The rear relay test point uses PB12 because PA14 is the
  table-assigned rear-left LED on that board.
- The servo PWM output is moved from PC11 to PA8 (TIM1_CH1). The motor PWM
  outputs are moved from PD2 to PB0/PB1 (TIM3_CH3/CH4), because PC11 and PD2
  do not have timer PWM channels on the STM32F401RE.
- The table's remaining test points are CAN_RX_A/B on PB8/PB9, CAN_TX_A/B on
  PA2/PA3, DEG_A/B on PC10/PC12, RELAY_TX on front PA14 (back PB12), and
  PWM_SET on back PC11. Front FL/FR are PD2/PA13, front ERR is PA15, and back
  RL/RR are PA14/PA15. CAN SPI chip selects and interrupts remain PB6/PC7 and
  PA10/PB5 as in `stm_can`.
- Additional back-only wiring is left/right encoder A/B on PC0-PC3 and the
  second motor direction input on PB7. Motor direction A remains on PA13.

PA13/PA14 are used as GPIOs by the table (front indicator/relay test point;
back direction/rear-left indicator), so using those pins may interfere with
live SWD debugging on the Nucleo. The UART console is TX-only on PA9.

Both apps keep UART1 TX on PA9 only for diagnostic console output. All
application messages are sent over CAN.

### CAN message format

The 11-bit CAN identifier contains the 3-bit Zone 1 message type in bits
10:8, the 7-bit sequence number in bits 7:1, and a relay marker in bit 0.
New messages use relay marker 0; forwarding across the other CAN bus sets it
to 1, preventing forwarding loops. The eight Zone 1 data bytes are carried
unchanged in the CAN payload, and CAN's frame CRC replaces the UART checksum.
Sequence numbers wrap at 128. External CAN senders must use this identifier
format and send 8-byte data frames.

Both boards relay each first-seen unmarked frame to the other bus and suppress
near-simultaneous duplicates received on both buses. Per-bus CAN health LEDs
indicate a non-active/error state or a transmit error. The front board reports
the zone error state; the rear board runs motor failsafe behavior locally.
Status current fields are zero because this pin assignment provides no current
sensor inputs.

Build either application with the board target:

```sh
west build -b nucleo_f401re/stm32f401xe stm32_front
west build -b nucleo_f401re/stm32f401xe stm32_back
```