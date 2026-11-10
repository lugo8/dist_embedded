#!/bin/sh
# Lab 5, every run on the Pi: bring up CAN, build cockpit, start it.
#   ./pi/run.sh            (from anywhere; it works from the repo root)
# Leaves raft_cockpit.log in the repo root, so a restarted Pi always finds its log.
set -e
cd "$(dirname "$0")/.."

sh pi/setup.sh

# raft.conf -> raft_conf.h, then the cockpit with its Raft and time-sync code
mkdir -p pi/build
python3 tools/gen_raft_conf.py raft.conf pi/build/raft_conf.h
gcc -std=gnu11 -O2 -Wall -Ipi/build -Icommon -o pi/cockpit \
	pi/cockpit.c pi/raft_node.c common/raft.c common/timesync.c -lpthread -llgpio

exec sudo ./pi/cockpit
