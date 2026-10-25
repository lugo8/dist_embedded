#!/bin/sh
# Pi 5 CAN setup.
#
# One time: add to /boot/firmware/config.txt, then reboot. The interrupt= pins must
# match the module INT wiring, and must not be any test-point pin in cockpit.c.
#   dtparam=spi=on
#   dtoverlay=mcp2515-can0,oscillator=16000000,interrupt=25
#   dtoverlay=mcp2515-can1,oscillator=16000000,interrupt=24
#
# Every boot: run this. restart-ms lets a bus recover from bus-off on its own (R4.8).
set -e
for i in can0 can1; do
	sudo ip link set "$i" down 2>/dev/null || true
	sudo ip link set "$i" txqueuelen 4 up type can bitrate 500000 restart-ms 100
done
ip -details link show can0 | grep -o "state [A-Z-]*\|restart-ms [0-9]*"
ip -details link show can1 | grep -o "state [A-Z-]*\|restart-ms [0-9]*"
