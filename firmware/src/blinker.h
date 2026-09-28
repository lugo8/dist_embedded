#pragma once
#include <stdbool.h>

void blinker_init(void);
void blinker_left(void);
void blinker_right(void);
void blinker_hazard(bool on);
void blinker_steer(int steer);