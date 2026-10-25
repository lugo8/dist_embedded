#!/usr/bin/env python3
"""
Checkoff step 2: record both buses and check every catalog ID's period, that it is on
both buses, and that its sequence counter never skips.

  python3 pi/check_bus.py            # records 10 s with candump (run cockpit + proxy first)
  python3 pi/check_bus.py 30         # records 30 s
  python3 pi/check_bus.py log.txt    # checks a saved `candump -L any` log

Turn a blinker on while it records so BLINK (0x200) is checked too.
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
    0x200: ("BLINK", 500, 0.10),
    0x300: ("FORCE", 50, 0.10),
}


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
        seq = int(data[:2], 16) & 0x0F
        frames.setdefault((bus_of(parts[1]), int(ident, 16)), []).append((t, seq))
    return frames


def check(frames):
    ok = True
    print(f"{'ID':<11}{'bus':<5}{'frames':>7}{'mean ms':>10}{'min':>8}{'max':>8}{'seq skips':>11}  result")
    for ident, (name, period, tol) in CATALOG.items():
        for bus in ("A", "B"):
            got = frames.get((bus, ident), [])
            label = f"{ident:03X} {name}"
            if len(got) < 3:
                if ident == 0x200:
                    print(f"{label:<11}{bus:<5}{len(got):>7}{'':>37}  skipped (turn a blinker on)")
                else:
                    print(f"{label:<11}{bus:<5}{len(got):>7}{'':>37}  FAIL: missing")
                    ok = False
                continue
            gaps = [(b[0] - a[0]) * 1000 for a, b in zip(got, got[1:])]
            if ident == 0x200:
                gaps = [g for g in gaps if g > period / 2]  # drop the extra frame sent on a change
            skips = sum((b[1] - a[1]) % 16 != 1 for a, b in zip(got, got[1:]))
            mean = statistics.mean(gaps)
            good = abs(mean - period) <= period * tol and skips == 0
            ok &= good
            print(f"{label:<11}{bus:<5}{len(got):>7}{mean:>10.1f}{min(gaps):>8.1f}{max(gaps):>8.1f}"
                  f"{skips:>11}  {'pass' if good else 'FAIL'}")
    faults = sum(len(frames.get((b, 0x020), [])) for b in ("A", "B"))
    print(f"\nFAULT (020) frames seen: {faults} (expect 0 unless you caused a fault)")
    print("ALL PASS" if ok else "SOMETHING FAILED (see above)")
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
