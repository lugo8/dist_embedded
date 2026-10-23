#pragma once

#include <stdint.h>

int motor_init(void);
int motor_forward(void);
int motor_reverse(void);
int motor_duty(uint32_t duty_pct);
int motor_brake(void);
int motor_brake_duty(uint32_t pct);
int motor_coast(void);
int motor_stop(void);
