#pragma once
#include <stdbool.h>
#include <stdint.h>

int blinker_init(void);
int blinker_set_message(uint8_t state, bool leds_on);

/* Feed the front zone's heartbeat seq (front: when sent, back: when received)
 * to phase-lock the no-RPi hazard blink on both zones.
 */
void blinker_hazard_sync(uint8_t hb_seq);
