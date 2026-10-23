#pragma once
#include <stdint.h>

int servo_init(void);
int servo_set_us(uint32_t us);
int servo_set_angle(int wheel_deg);