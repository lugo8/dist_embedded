#pragma once

int encoder_init(void);
void encoder_rpm(int period_ms, int *left, int *right);