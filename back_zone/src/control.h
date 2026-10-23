#ifndef CONTROL_H_
#define CONTROL_H_

#include <stdbool.h>

#include "protocol.h"

#define ENABLE_ENCODER_FEEDBACK 0

#define CONTROL_PERIOD_MS 10

#define WIRE_AXIS_MAX 32767
#define WHEEL_THROTTLE_MAX 1000
#define WHEEL_BRAKE_MAX 1000
#define MAX_TARGET_RPM 230
#define THROTTLE_DEADZONE 50
#define COAST_DECAY_RPM_PER_TICK 0.5f

#define FF_TABLE { \
	{ 143, 60 }, \
	{ 208, 70 }, \
	{ 266, 80 }, \
	{ 302, 90 }, \
	{ 315, 100 }, \
}

#define PID_KP 0.2f
#define PID_KI 0.01f
#define PID_KD 0.0f
#define PID_I_LIMIT_PCT 30.0f
#define PID_I_ZONE_RPM 30.0f
#define PID_OUT_LIMIT_PCT 20.0f

#define HOLD_ENTER_RPM 5
#define HOLD_ENTER_COUNTS 8
#define HOLD_EXIT_COUNTS 3
#define HOLD_REVERSE_LOCKOUT_MS 50
#define HOLD_MIN_DUTY_PCT 55
#define HOLD_KP 0.5f
#define HOLD_KI 0.0f
#define HOLD_KD 0.0f
#define HOLD_I_LIMIT_PCT 15.0f
#define HOLD_I_ZONE_COUNTS 30.0f
#define HOLD_OUT_LIMIT_PCT 20.0f

void control_start(bool encoder_available);
int control_apply_zone_control(const struct wheel_state *ws);

#endif
