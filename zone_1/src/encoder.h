#pragma once

#include <stdint.h>

int encoder_init(void);
void encoder_rpm(int period_ms, int *left, int *right);
/* signed encoder counts since boot (forward = positive on both wheels), updated by encoder_rpm() */
void encoder_position(int32_t *left, int32_t *right);
