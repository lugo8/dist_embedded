#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include "servo.h"

/* steering linkage limits */
#define SERVO_MIN_US 920    /* full left */
#define SERVO_MAX_US 2040   /* full right */
/* lab 1 wheel range */
#define WHEEL_MAX_DEG 900

static const struct pwm_dt_spec servo = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));

int servo_init(void)
{
	return pwm_is_ready_dt(&servo) ? 0 : -ENODEV;
}

/* clamp here so nothing can command past the mechanical stops */
uint32_t servo_set_us(uint32_t us)
{
	us = CLAMP(us, SERVO_MIN_US, SERVO_MAX_US);
	pwm_set_pulse_dt(&servo, PWM_USEC(us));
	return us;
}

/* linear map so the whole wheel range covers the whole linkage range */
uint32_t servo_set_angle(int wheel_deg)
{
	wheel_deg = CLAMP(wheel_deg, 0, WHEEL_MAX_DEG);
	return servo_set_us(SERVO_MIN_US +
			    (uint32_t)wheel_deg * (SERVO_MAX_US - SERVO_MIN_US) / WHEEL_MAX_DEG);
}