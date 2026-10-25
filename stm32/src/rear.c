/* N3 (drivetrain): DRIVE and PID */
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include "zone.h"

#define USER DT_PATH(zephyr_user)

static const struct pwm_dt_spec motor = PWM_DT_SPEC_GET(USER);
static const struct gpio_dt_spec dir_a = GPIO_DT_SPEC_GET(USER, dir_a_gpios);
static const struct gpio_dt_spec pwm_set_pin = GPIO_DT_SPEC_GET(USER, pwm_set_gpios);

static K_SEM_DEFINE(ctrl_sem, 0, 1);
static K_MUTEX_DEFINE(ctrl_lock);
static struct ctrl latest;
static uint8_t duty_pct; /* setpoint DRIVE hands to PID */

void actuator_ctrl(const struct ctrl *c)
{
	k_mutex_lock(&ctrl_lock, K_FOREVER);
	latest = *c;
	k_mutex_unlock(&ctrl_lock);
	k_sem_give(&ctrl_sem);
}

/* entering error: wake DRIVE so the motor brakes now, not on the next CTRL */
void actuator_error(void)
{
	k_sem_give(&ctrl_sem);
}

static void set_duty(uint8_t pct)
{
	pwm_set_pulse_dt(&motor, motor.period / 100 * pct);
}

/* DRIVE: brake in error or on brake pedal; DIR_A high = forward, low = braking */
static void drive_task(void *p1, void *p2, void *p3)
{
	while (1) {
		k_sem_take(&ctrl_sem, K_FOREVER);
		k_mutex_lock(&ctrl_lock, K_FOREVER);
		bool brake = monitor_in_error() || latest.brake > 0;
		uint8_t duty = brake ? 0 : latest.throttle;
		duty_pct = duty;
		k_mutex_unlock(&ctrl_lock);

		gpio_pin_set_dt(&dir_a, !brake);
		set_duty(duty);
		gpio_pin_toggle_dt(&pwm_set_pin);
	}
}

/* PID: 10 ms. No encoders in this lab, so it only holds the setpoint;
 * the Lab 2 speed loop goes here to drive the real car. */
static void pid_task(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get();

	while (1) {
		k_mutex_lock(&ctrl_lock, K_FOREVER);
		uint8_t duty = duty_pct;
		k_mutex_unlock(&ctrl_lock);

		set_duty(duty);
		next += PID_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

K_THREAD_DEFINE(drive_tid, 1024, drive_task, NULL, NULL, NULL, PRIO_ACT, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(pid_tid, 1024, pid_task, NULL, NULL, NULL, PRIO_PID, 0, SYS_FOREVER_MS);

int actuator_init(void)
{
	if (!pwm_is_ready_dt(&motor) || !gpio_is_ready_dt(&dir_a) ||
	    gpio_pin_configure_dt(&dir_a, GPIO_OUTPUT_INACTIVE) ||
	    gpio_pin_configure_dt(&pwm_set_pin, GPIO_OUTPUT_INACTIVE)) {
		return -ENODEV;
	}
	set_duty(0);
	k_thread_start(drive_tid);
	k_thread_start(pid_tid);
	return 0;
}
