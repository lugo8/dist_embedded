#!/usr/bin/env python3
"""
Seeds for default_leader = random (checkoff step 8). Every node starts its election timer
when the cluster forms, the latecomer (the Pi) up to ~20 ms later or earlier than the others.
A seed "makes X win" when X's first election timeout is shorter than everyone else's by
more than that.   find_seed.py [raft.conf] [--margin MS] [--count N]
"""
import argparse
import re

M32 = 0xFFFFFFFF


def rand_init(seed, me):  # same as raft_rand_init: murmur3 finalizer over (seed, node)
    s = (seed ^ (me * 0x9E3779B9)) & M32
    s ^= s >> 16
    s = (s * 0x85EBCA6B) & M32
    s ^= s >> 13
    s = (s * 0xC2B2AE35) & M32
    s ^= s >> 16
    return s or 0xA5A5A5A5


def rand_next(s):  # xorshift32, same as raft_rand_next
    s ^= (s << 13) & M32
    s ^= s >> 17
    s ^= (s << 5) & M32
    return s


def first_jitter(seed, me, jitter):
    return rand_next(rand_init(seed, me)) % (jitter + 1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("conf", nargs="?", default="raft.conf")
    ap.add_argument("--margin", type=int, default=25)
    ap.add_argument("--count", type=int, default=3)
    a = ap.parse_args()
    text = open(a.conf).read()
    jitter = int(re.search(r"^election_timeout_jitter_ms\s*=\s*(\d+)", text, re.M).group(1))
    names = [s.strip() for s in re.search(r"^nodes\s*=\s*([^#\n]+)", text, re.M).group(1).split(",")]
    for winner in (1, 2, 3):
        found = []
        for seed in range(1, 100000):
            j = {n: first_jitter(seed, n, jitter) for n in (1, 2, 3)}
            if all(j[winner] + a.margin < j[n] for n in j if n != winner):
                found.append((seed, j))
            if len(found) == a.count:
                break
        print(f"{names[winner - 1]} (N{winner}) wins:")
        for seed, j in found:
            print(f"  seed = {seed:<6} jitter ms: " + ", ".join(f"N{n} {j[n]}" for n in j))


if __name__ == "__main__":
    main()
