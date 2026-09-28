#pragma once
#include <stdint.h>

int motor_init(void);
void motor_forward(void);
void motor_reverse(void);
void motor_duty(uint32_t left_pct, uint32_t right_pct);
void motor_brake(void);
void motor_coast(void);