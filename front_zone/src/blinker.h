#pragma once
#include <stdbool.h>
#include <stdint.h>

int blinker_init(void);
int blinker_set_message(uint8_t state, bool leds_on);
void blinker_hazard_sync(uint8_t heartbeat_sequence);