/*
 * Routing from the network state (Lab 5 Part 5): who relays between the buses.
 *
 * Lab 3 relaying was forward-on-detect: copy a frame from zone S to the other bus once S
 * has been missing there for 30 ms. N2 relayed; N3 only when it could not hear N2's
 * heartbeat on both buses; the Pi never. Exactly one relay, so two relays never put the
 * same frame on a bus where only they are listening (nobody would ACK: the bus jams).
 *
 * Lab 5 keeps that rule, because it hands over within one missed heartbeat (30 ms, well
 * inside the 65 ms heartbeat timeout), and adds two things:
 *   - the Pi as the last resort, when neither STM32 is on both buses: checkoff step 9
 *     (an STM32 leading, the other cut off) has no other way between the STM32s;
 *   - the Raft network graph: a node the committed graph has off a bus is not a relay,
 *     on every node alike, until the graph has it back.
 *
 *   relay = the first of N2, N3, N1 that the graph has on both buses and whose own
 *           heartbeat this node has heard directly on both buses in the last 30 ms
 *           (for itself: that it is sending heartbeats)
 *
 * A first try picked the lowest-numbered healthy node from the heartbeats' DEG bits. That
 * made the Pi the relay by default and took 50-70 ms to hand over when the Pi lost a bus,
 * past the 65 ms heartbeat timeout: zones dropped into the error state at every link pull.
 */
#ifndef ROUTE_H
#define ROUTE_H

#include "raft.h"

#define ROUTE_HB_MS 30 /* a relay candidate is missing from a bus after 1.5 heartbeats */

/* hb_both[x]: x's heartbeat heard directly on both buses in the last ROUTE_HB_MS */
static inline uint8_t route_relay(const struct net_state *net, const bool hb_both[4],
				  uint8_t me, bool my_hb)
{
	static const uint8_t order[] = { ZONE_N2, ZONE_N3, ZONE_N1 };

	for (unsigned i = 0; i < sizeof(order); i++) {
		uint8_t x = order[i];

		if (!net->node_up[x] || !net->link_up[x][BUS_A] || !net->link_up[x][BUS_B]) {
			continue;
		}
		if (x == me ? my_hb : hb_both[x]) {
			return x;
		}
	}
	return ZONE_NONE;
}

#endif /* ROUTE_H */
