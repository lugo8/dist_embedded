#include <zephyr/kernel.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/drivers/gpio.h>
#include "motor.h"

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec ena = PWM_DT_SPEC_GET_BY_IDX(USER, 1);
static const struct pwm_dt_spec enb = PWM_DT_SPEC_GET_BY_IDX(USER, 2);
static const struct gpio_dt_spec in1 = GPIO_DT_SPEC_GET(USER, in1_gpios);
static const struct gpio_dt_spec in2 = GPIO_DT_SPEC_GET(USER, in2_gpios);
static const struct gpio_dt_spec tp_pwm_set = GPIO_DT_SPEC_GET(USER, tp_pwm_set_gpios);

int motor_init(void)
{
	if (!pwm_is_ready_dt(&ena) || !pwm_is_ready_dt(&enb) ||
	    !gpio_is_ready_dt(&in1) || !gpio_is_ready_dt(&in2) ||
	    !gpio_is_ready_dt(&tp_pwm_set)) {
		return -ENODEV;
	}
	gpio_pin_configure_dt(&in1, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&in2, GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&tp_pwm_set, GPIO_OUTPUT_INACTIVE);
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

/* every duty change goes through here so PWM_SET toggles right after the timer write */
void motor_duty(uint32_t left_pct, uint32_t right_pct)
{
	pwm_set_pulse_dt(&ena, ena.period * MIN(left_pct, 100) / 100);
	pwm_set_pulse_dt(&enb, enb.period * MIN(right_pct, 100) / 100);
	gpio_pin_toggle_dt(&tp_pwm_set);
}

/* both inputs low with enable fully on shorts the motor leads, so it stops fast.
 * in1 drops from 1 to 0 here when coming from forward, which gives dir_a an edge */
void motor_brake(void)
{
	motor_brake_duty(100);
}

/* same shorted-leads brake, but only for pct% of each pwm period (the rest coasts),
 * so the braking force scales with pct */
void motor_brake_duty(uint32_t pct)
{
	gpio_pin_set_dt(&in1, 0);
	gpio_pin_set_dt(&in2, 0);
	motor_duty(pct, pct);
}

/* enable off disconnects the motor so it spins down on its own */
void motor_coast(void)
{
	motor_duty(0, 0);
}