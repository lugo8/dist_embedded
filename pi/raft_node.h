/* Lab 5 on the Pi (N1): Raft agent + client, time sync, PPS, LEADER/SYNC LEDs, log file */
#ifndef RAFT_NODE_H
#define RAFT_NODE_H

#include <stdbool.h>
#include <stdint.h>
#include "../common/can_msg.h"

/* BCM numbers (header pin); none of them may be an MCP2515 INT pin in config.txt */
#define PIN_PPS    17 /* pin 11 */
#define PIN_LEADER 23 /* pin 16 */
#define PIN_SYNC   12 /* pin 32 */
#define PIN_REF_TX 20 /* pin 38: toggles on every REF this node sends */

struct raft_node_hooks {
	void (*send_both)(uint16_t id, uint8_t d[MSG_DLC]); /* seq handled by the hook */
	void (*send_bus)(int bus, uint16_t id, const uint8_t d[MSG_DLC]);
	void (*gpio)(int pin, int v);
	uint8_t (*deg_bits)(void);
	bool (*sending_hb)(void);
	bool (*hb_both)(uint8_t zone); /* zone's heartbeat heard directly on both buses lately */
};

void raft_node_init(const struct raft_node_hooks *h, const char *log_path);
void raft_node_start(void);
void raft_node_hb_sent(void); /* call after each cockpit heartbeat is sent */

/* every accepted frame, both copies; rx_us on pi_local_us()'s clock */
void raft_node_rx(int bus, uint16_t id, const uint8_t d[MSG_DLC], bool relayed, bool first,
		  int64_t rx_us);
/* our own REF came back from the driver: that copy is on the wire */
void raft_node_ref_echo(int bus, const uint8_t d[MSG_DLC], int64_t tx_us);

void raft_node_event(uint8_t app_kind);
uint8_t raft_node_sync_bits(void);
uint8_t raft_node_relay(void); /* the zone that relays (route.h) */

int64_t pi_local_us(void);      /* CLOCK_MONOTONIC_RAW in us */
int64_t pi_global_gettime(void);

#endif /* RAFT_NODE_H */
