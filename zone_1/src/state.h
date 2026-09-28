#ifndef STATE_H_
#define STATE_H_

#include <stdbool.h>

#include "protocol.h"

// timing stuff
#define LINK_TIMEOUT_MS 150

void state_set_wheel_state(const struct wheel_state *ws);
void state_get_wheel_state(struct wheel_state *out);

void state_set_hb_rpi(const struct heartbeat *hb);
void state_set_hb_drivetrain(const struct heartbeat *hb);

void state_set_force_feedback(const struct force_feedback *fb);

bool link_is_alive(void);

/* zone_state as reported in the status frame (enum zone_state) */
void state_set_zone_state(uint8_t zone_state);
uint8_t state_get_zone_state(void);

#endif /* STATE_H_ */
