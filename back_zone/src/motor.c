#include "motor.h"

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec motor_pwm =
	PWM_DT_SPEC_GET_BY_IDX(USER, 0);
static const struct gpio_dt_spec dir_a = GPIO_DT_SPEC_GET(USER, dir_a_gpios);
static const struct gpio_dt_spec dir_b = GPIO_DT_SPEC_GET(USER, dir_b_gpios);
static const struct gpio_dt_spec pwm_set_pin =
	GPIO_DT_SPEC_GET(USER, tp_pwm_set_gpios);

static bool motor_ready;
K_MUTEX_DEFINE(motor_lock);

static int set_direction(bool a, bool b)
{
	int rc = gpio_pin_set_dt(&dir_a, a);

	if (rc) {
		return rc;
	}

	return gpio_pin_set_dt(&dir_b, b);
}

static int write_duty(uint32_t duty_pct)
{
	duty_pct = MIN(duty_pct, 100U);

	int rc = pwm_set_pulse_dt(&motor_pwm,
				  motor_pwm.period * duty_pct / 100U);
	if (rc) {
		return rc;
	}

	//printk("motor duty: %u%%\n", duty_pct);
	return gpio_pin_toggle_dt(&pwm_set_pin);
}

static int coast_locked(void)
{
	return write_duty(0);
}

static int brake_duty_locked(uint32_t pct)
{
	int rc = set_direction(false, false);

	return rc ? rc : write_duty(pct);
}

int motor_init(void)
{
	if (!pwm_is_ready_dt(&motor_pwm) ||
	    !gpio_is_ready_dt(&dir_a) || !gpio_is_ready_dt(&dir_b) ||
	    !gpio_is_ready_dt(&pwm_set_pin)) {
		return -ENODEV;
	}

	int rc = gpio_pin_configure_dt(&dir_a, GPIO_OUTPUT_INACTIVE);

	if (rc) {
		return rc;
	}
	rc = gpio_pin_configure_dt(&dir_b, GPIO_OUTPUT_INACTIVE);
	if (rc) {
		return rc;
	}
	rc = gpio_pin_configure_dt(&pwm_set_pin, GPIO_OUTPUT_INACTIVE);
	if (rc) {
		return rc;
	}

	k_mutex_lock(&motor_lock, K_FOREVER);
	rc = coast_locked();
	if (!rc) {
		motor_ready = true;
	}
	k_mutex_unlock(&motor_lock);

	return rc;
}

int motor_forward(void)
{
	k_mutex_lock(&motor_lock, K_FOREVER);
	int rc = set_direction(true, false);

	k_mutex_unlock(&motor_lock);
	return rc;
}

int motor_reverse(void)
{
	k_mutex_lock(&motor_lock, K_FOREVER);
	int rc = set_direction(false, true);

	k_mutex_unlock(&motor_lock);
	return rc;
}

int motor_duty(uint32_t duty_pct)
{
	k_mutex_lock(&motor_lock, K_FOREVER);
	int rc = write_duty(duty_pct);

	k_mutex_unlock(&motor_lock);
	return rc;
}

int motor_brake_duty(uint32_t pct)
{
	k_mutex_lock(&motor_lock, K_FOREVER);
	int rc = brake_duty_locked(MIN(pct, 100U));

	k_mutex_unlock(&motor_lock);
	return rc;
}

int motor_brake(void)
{
	return motor_brake_duty(100);
}

int motor_coast(void)
{
	k_mutex_lock(&motor_lock, K_FOREVER);
	int rc = coast_locked();

	k_mutex_unlock(&motor_lock);
	return rc;
}

int motor_stop(void)
{
	if (!motor_ready) {
		return 0;
	}

	return motor_brake();
}
