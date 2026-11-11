/* generated from /Users/leesiena/Desktop/18-649/dist_embedded/stm32/../raft.conf by tools/gen_raft_conf.py; edit raft.conf, not this file */
#ifndef RAFT_CONF_H
#define RAFT_CONF_H

#define RAFT_LEADER_ELECT 0 /* default_leader = random / none */

#define RAFT_CONF_N                 3
#define RAFT_CONF_DEFAULT_LEADER    RAFT_LEADER_ELECT /* random */
#define RAFT_CONF_SEED              48u
#define RAFT_CONF_HEARTBEAT_MS      20
#define RAFT_CONF_MISSED_HEARTBEATS 3
#define RAFT_CONF_JITTER_MS         39
#define RAFT_CONF_LOG_SIZE          30000
#define RAFT_CONF_DEBUG             1
#define RAFT_CONF_NODE_NAMES        { "none", "cockpit", "front", "rear" }

#endif /* RAFT_CONF_H */
