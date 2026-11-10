#!/usr/bin/env python3
"""
Checkoff step 2 (Lab 5): record both buses and check every catalog ID's period, that it is
on both buses, and that its sequence counter never skips; REF from the leader every 20 ms
with its cycle counter going up by one, an empty APPEND every 100 ms, and no REQ_VOTE.

  python3 pi/check_bus.py            # records 10 s with candump (run cockpit + proxy first)
  python3 pi/check_bus.py 30         # records 30 s
  python3 pi/check_bus.py log.txt    # checks a saved `candump -L any` log

Relayed copies (byte 0 flag 0x10) are left out of the timing: they are extra copies.
"""
import os
import statistics
import subprocess
import sys

# id: (name, period ms, allowed +/- fraction on the mean)
CATALOG = {
    0x010: ("CTRL", 20, 0.25),   # follows the wheel packets, so it wobbles more
    0x100: ("HB_N1", 20, 0.10),  # R4.5: 20 ms +/- 10 %
    0x101: ("HB_N2", 20, 0.10),
    0x102: ("HB_N3", 20, 0.10),
    0x300: ("FORCE", 50, 0.10),
}
NODES = {0: "N1", 1: "N2", 2: "N3"}
REF_BASE, VOTE_REQ_BASE, APPEND_BASE = 0x030, 0x040, 0x050
FLAG_RELAYED = 0x10


def bus_of(ifname):
    """bus A is the chip on CE0 (spi0.0), whatever Linux named it"""
    try:
        link = os.readlink(f"/sys/class/net/{ifname}/device")
        return "A" if link.endswith("spi0.0") else "B"
    except OSError:
        return {"can0": "A", "can1": "B"}.get(ifname, ifname)


def read_log(lines):
    """candump -L lines: (1698.123456) can0 100#0800000000000000"""
    frames = {}  # (bus, id) -> [(t, seq)]
    for line in lines:
        parts = line.split()
        if len(parts) != 3 or "#" not in parts[2]:
            continue
        t = float(parts[0].strip("()"))
        ident, data = parts[2].split("#")
        if len(ident) > 3 or not data:
            continue  # extended frames, or no payload
        payload = bytes.fromhex(data)
        if payload[0] & FLAG_RELAYED:
            continue
        frames.setdefault((bus_of(parts[1]), int(ident, 16)), []).append((t, payload[0] & 0x0F, payload))
    return frames


def check(frames):
    ok = True
    print(f"{'ID':<11}{'bus':<5}{'frames':>7}{'mean ms':>10}{'min':>8}{'max':>8}{'seq skips':>11}  result")
    for ident, (name, period, tol) in CATALOG.items():
        for bus in ("A", "B"):
            got = frames.get((bus, ident), [])
            label = f"{ident:03X} {name}"
            if len(got) < 3:
                print(f"{label:<11}{bus:<5}{len(got):>7}{'':>37}  FAIL: missing")
                ok = False
                continue
            gaps = [(b[0] - a[0]) * 1000 for a, b in zip(got, got[1:])]
            skips = sum((b[1] - a[1]) % 16 != 1 for a, b in zip(got, got[1:]))
            mean = statistics.mean(gaps)
            good = abs(mean - period) <= period * tol and skips == 0
            ok &= good
            print(f"{label:<11}{bus:<5}{len(got):>7}{mean:>10.1f}{min(gaps):>8.1f}{max(gaps):>8.1f}"
                  f"{skips:>11}  {'pass' if good else 'FAIL'}")
    ok &= check_raft(frames)
    faults = sum(len(frames.get((b, 0x020), [])) for b in ("A", "B"))
    print(f"\nFAULT (020) frames seen: {faults} (expect 0 unless you caused a fault)")
    print("ALL PASS" if ok else "SOMETHING FAILED (see above)")
    return ok


def check_raft(frames):
    ok = True
    print()
    for bus in ("A", "B"):
        refs = {i: frames.get((bus, REF_BASE + i), []) for i in NODES}
        senders = [i for i in refs if refs[i]]
        if not senders:
            print(f"REF        {bus:<5}      0  FAIL: no REF on this bus")
            ok = False
            continue
        if len(senders) > 1:
            print(f"REF        {bus:<5}  REF from {', '.join(NODES[i] for i in senders)}: leader changed while recording")
        for i in senders:
            got = refs[i]
            gaps = [(b[0] - a[0]) * 1000 for a, b in zip(got, got[1:])]
            cyc = [p[2] for _, _, p in got]
            cyc_skips = sum((b - a) % 256 != 1 for a, b in zip(cyc, cyc[1:]))
            mean = statistics.mean(gaps) if gaps else 0
            good = len(got) > 2 and abs(mean - 20) <= 2 and cyc_skips == 0
            ok &= good
            print(f"{REF_BASE + i:03X} REF_{NODES[i]} {bus:<5}{len(got):>7}{mean:>10.1f}{min(gaps or [0]):>8.1f}"
                  f"{max(gaps or [0]):>8.1f}{cyc_skips:>11}  {'pass' if good else 'FAIL'} (cycle skips)")
            for t, _, p in got[:3]:
                term, cycle = p[1], p[2]
                master = int.from_bytes(p[3:7], "little")
                fu = " follow-up" if p[0] & 0x20 else ""
                print(f"           cycle {cycle:3d}  master time {master / 1e6:12.6f} s  term {term}"
                      f"  mode {p[7]}{fu}")
        appends = []
        for i in NODES:
            appends += [(t, p) for t, _, p in frames.get((bus, APPEND_BASE + i), [])]
        empty = sorted(t for t, p in appends if p[6] == 0)
        gaps = [(b - a) * 1000 for a, b in zip(empty, empty[1:])]
        mean = statistics.mean(gaps) if gaps else 0
        good = len(empty) > 2 and 90 <= mean <= 110
        ok &= good
        print(f"05x APPEND {bus:<5}{len(empty):>7}{mean:>10.1f}{'':>27}  {'pass' if good else 'FAIL'}"
              f" (empty, every 100 ms; {len(appends) - len(empty)} with an entry)")
        votes = sum(len(frames.get((bus, VOTE_REQ_BASE + i), [])) for i in NODES)
        good = votes == 0
        ok &= good
        print(f"04x REQ_VOTE {bus:<3}{votes:>7}{'':>37}  {'pass' if good else 'FAIL: an election happened'}")
    return ok


if __name__ == "__main__":
    arg = sys.argv[1] if len(sys.argv) > 1 else "10"
    if os.path.isfile(arg):
        with open(arg) as f:
            lines = f.readlines()
    else:
        print(f"recording both buses for {arg} s...")
        out = subprocess.run(["timeout", arg, "candump", "-L", "any"], capture_output=True, text=True)
        lines = out.stdout.splitlines()
    sys.exit(0 if check(read_log(lines)) else 1)
