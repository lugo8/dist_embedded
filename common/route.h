/*
 * Routing from the network state (Lab 5 Part 5).
 *
 * Lab 3 relaying was forward-on-detect with a hard-coded "who relays" rule (N2 first, N3
 * only when N2 is missing, the Pi never). Lab 5 keeps the per-frame forward-on-detect test
 * (copy a frame from S to the other bus when S's own heartbeat is missing there) and takes
 * the "who relays" decision from the Raft network graph, which every node holds the same:
 *
 *   the relay is the lowest-numbered node that the graph has up on both buses and that is
 *   not reporting either bus degraded in its last heartbeat.
 *
 * Exactly one node relays, so two relays never put the same frame on a bus where only they
 * are listening (the Lab 3 jam), and the Pi now relays too: it is the only node left on both
 * buses in checkoff step 9 (an STM32 leading, the other STM32 cut off by scenario (c)).
 * Health bits come from heartbeats (20 ms), so a fault is routed around before the log
 * commits it (Part 5.2); the graph takes over once it does.
 */
#ifndef ROUTE_H
#define ROUTE_H

#include "raft.h"

#define ROUTE_HEALTH_FRESH_MS 65 /* a heartbeat's DEG bits count this long */

struct route_health {
	uint8_t deg[RAFT_NODES + 1];      /* HEALTH_A_DEG | HEALTH_B_DEG from the last HB */
	uint32_t hb_ms[RAFT_NODES + 1];   /* when that HB arrived, 0 = never */
};

/*
 * The relay. `my_deg` is this node's own degraded bits, `my_hb` whether it is sending
 * heartbeats right now: a node judges itself exactly as the others judge it from its
 * heartbeats, so every node picks the same relay.
 */
static inline uint8_t route_relay(const struct net_state *net, const struct route_health *h,
				  uint8_t me, uint8_t my_deg, bool my_hb, uint32_t now)
{
	uint8_t fallback = ZONE_NONE;

	for (uint8_t x = ZONE_N1; x <= ZONE_N3; x++) {
		if (!net->node_up[x] || !net->link_up[x][BUS_A] || !net->link_up[x][BUS_B]) {
			continue;
		}
		bool known = x == me ? my_hb : (h->hb_ms[x] && now - h->hb_ms[x] < ROUTE_HEALTH_FRESH_MS);
		uint8_t deg = x == me ? my_deg : h->deg[x];

		if (known && !deg) {
			return x;
		}
		if (!known && fallback == ZONE_NONE) {
			fallback = x; /* no fresh heartbeat (e.g. the Pi with the proxy stopped) */
		}
	}
	return fallback;
}

#endif /* ROUTE_H */
