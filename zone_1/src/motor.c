#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include "motor.h"

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec ena = PWM_DT_SPEC_GET_BY_IDX(USER, 1);
static const struct pwm_dt_spec enb = PWM_DT_SPEC_GET_BY_IDX(USER, 2);
static const struct gpio_dt_spec in1 = GPIO_DT_SPEC_GET(USER, in1_gpios);
static const struct gpio_dt_spec in2 = GPIO_DT_SPEC_GET(USER, in2_gpios);

int motor_init(void)
{
	if (!pwm_is_ready_dt(&ena) || !pwm_is_ready_dt(&enb) ||
	    !gpio_is_ready_dt(&in1) || !gpio_is_ready_dt(&in2)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&in1, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&in2, GPIO_OUTPUT_INACTIVE);
	motor_coast();
	return 0;
}

void motor_forward(void)
{
	gpio_pin_set_dt(&in1, 1);
	gpio_pin_set_dt(&in2, 0);
}

void motor_reverse(void)
{
	gpio_pin_set_dt(&in1, 0);
	gpio_pin_set_dt(&in2, 1);
}

void motor_duty(uint32_t left_pct, uint32_t right_pct)
{
	pwm_set_pulse_dt(&ena, ena.period * MIN(left_pct, 100) / 100);
	pwm_set_pulse_dt(&enb, enb.period * MIN(right_pct, 100) / 100);
}

/* both inputs low with enable fully on shorts the motor leads, so it stops fast.
 * in1 drops from 1 to 0 here when coming from forward, which gives dir_a an edge */
void motor_brake(void)
{
	gpio_pin_set_dt(&in1, 0);
	gpio_pin_set_dt(&in2, 0);
	pwm_set_pulse_dt(&ena, ena.period);
	pwm_set_pulse_dt(&enb, enb.period);
}

/* enable off disconnects the motor so it spins down on its own */
void motor_coast(void)
{
	pwm_set_pulse_dt(&ena, 0);
	pwm_set_pulse_dt(&enb, 0);
}