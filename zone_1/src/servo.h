#pragma once
#include <stdint.h>

int servo_init(void);
uint32_t servo_set_us(uint32_t us);
uint32_t servo_set_angle(int wheel_deg);