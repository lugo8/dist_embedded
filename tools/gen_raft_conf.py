#!/usr/bin/env python3
"""raft.conf -> raft_conf.h. Usage: gen_raft_conf.py raft.conf out.h"""
import sys

KEYS = {"N", "nodes", "default_leader", "seed", "heartbeat_ms", "missed_heartbeats",
        "election_timeout_jitter_ms", "log_size", "Debug"}


def parse(path):
    conf = {}
    for n, line in enumerate(open(path), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            sys.exit(f"{path}:{n}: expected key = value")
        k, v = (s.strip() for s in line.split("=", 1))
        if k not in KEYS:
            sys.exit(f"{path}:{n}: unknown key {k}")
        conf[k] = v
    missing = KEYS - conf.keys()
    if missing:
        sys.exit(f"{path}: missing {', '.join(sorted(missing))}")
    return conf


def main():
    src, out = sys.argv[1], sys.argv[2]
    c = parse(src)
    nodes = [s.strip() for s in c["nodes"].split(",")]
    n = int(c["N"])
    if n != 3 or len(nodes) != n:
        sys.exit("this build supports N = 3 with three node names")
    dl = c["default_leader"]
    if dl in ("random", "none"):
        leader = "RAFT_LEADER_ELECT"
    elif dl in nodes:
        leader = str(nodes.index(dl) + 1)
    elif dl.isdigit() and 1 <= int(dl) <= n:
        leader = dl
    else:
        sys.exit(f"default_leader must be one of {nodes}, random or none")
    ints = {k: int(c[k], 0) for k in ("seed", "heartbeat_ms", "missed_heartbeats",
                                       "election_timeout_jitter_ms", "log_size")}
    if not 1 <= ints["log_size"] <= 65000:
        sys.exit("log_size must fit a 16-bit index")
    debug = c["Debug"].lower() in ("yes", "true", "1", "on")
    names = ", ".join(f'"{s}"' for s in nodes)
    with open(out, "w") as f:
        f.write(f"""/* generated from {src} by tools/gen_raft_conf.py; edit raft.conf, not this file */
#ifndef RAFT_CONF_H
#define RAFT_CONF_H

#define RAFT_LEADER_ELECT 0 /* default_leader = random / none */

#define RAFT_CONF_N                 {n}
#define RAFT_CONF_DEFAULT_LEADER    {leader} /* {dl} */
#define RAFT_CONF_SEED              {ints['seed']}u
#define RAFT_CONF_HEARTBEAT_MS      {ints['heartbeat_ms']}
#define RAFT_CONF_MISSED_HEARTBEATS {ints['missed_heartbeats']}
#define RAFT_CONF_JITTER_MS         {ints['election_timeout_jitter_ms']}
#define RAFT_CONF_LOG_SIZE          {ints['log_size']}
#define RAFT_CONF_DEBUG             {int(debug)}
#define RAFT_CONF_NODE_NAMES        {{ "none", {names} }}

#endif /* RAFT_CONF_H */
""")


if __name__ == "__main__":
    main()
