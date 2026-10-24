#pragma once
#include <stdbool.h>
#include <stdint.h>

int blinker_init(void);
int blinker_set_message(uint8_t state, bool leds_on);

/* Heartbeat sources the no-RPi hazard blink can phase-lock to. */
enum hb_src { HB_SRC_FRONT, HB_SRC_BACK, HB_SRC_COUNT };

/* Feed a zone's heartbeat seq (the zone's own, once it was actually sent, or the other zone's, when received)
 * to phase-lock the no-RPi hazard blink on both zones. Uses the front's heartbeat while it is alive and the
 * back's when it is not.
 */
void blinker_hazard_sync(enum hb_src src, uint8_t hb_seq);

/* True while BLINKER frames from the RPi are still arriving. */
bool blinker_remote_fresh(void);
