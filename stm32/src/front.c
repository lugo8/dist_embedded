/* N2 (steering): STEER and FORCE_TX */
#include <zephyr/drivers/pwm.h>
#include "zone.h"

static const struct pwm_dt_spec servo = PWM_DT_SPEC_GET(DT_PATH(zephyr_user));

static K_SEM_DEFINE(ctrl_sem, 0, 1);
static K_MUTEX_DEFINE(ctrl_lock);
static struct ctrl latest;

void actuator_ctrl(const struct ctrl *c)
{
	k_mutex_lock(&ctrl_lock, K_FOREVER);
	latest = *c;
	k_mutex_unlock(&ctrl_lock);
	k_sem_give(&ctrl_sem);
}

void actuator_error(void)
{
	/* steering keeps following the wheel in the error state, unless this zone's
	 * own test button failed it (STEER holds the servo then) */
}

/* STEER: -450..450 deg -> 1..2 ms servo pulse */
static void steer_task(void *p1, void *p2, void *p3)
{
	while (1) {
		k_sem_take(&ctrl_sem, K_FOREVER);
		if (monitor_self_test()) {
			continue; /* this zone failed itself: hold the servo (R3) */
		}
		k_mutex_lock(&ctrl_lock, K_FOREVER);
		int steer = latest.steer;
		k_mutex_unlock(&ctrl_lock);

		pwm_set_pulse_dt(&servo, PWM_USEC(1500 + steer * 500 / STEER_MAX));
	}
}

/* FORCE_TX: 50 ms, payload is a dummy counter this lab */
static void force_task(void *p1, void *p2, void *p3)
{
	static atomic_t seq;
	int8_t count = 0;
	int64_t next = k_uptime_get();

	while (1) {
		next += FORCE_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
		uint8_t d[MSG_DLC] = { 0, (uint8_t)count++ };
		send_both(ID_FORCE, &seq, d);
	}
}

K_THREAD_DEFINE(steer_tid, 1024, steer_task, NULL, NULL, NULL, PRIO_ACT, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(force_tid, 1024, force_task, NULL, NULL, NULL, PRIO_FORCE, 0, SYS_FOREVER_MS);

int actuator_init(void)
{
	if (!pwm_is_ready_dt(&servo) || pwm_set_pulse_dt(&servo, PWM_USEC(1500))) {
		return -ENODEV;
	}
	k_thread_start(steer_tid);
	k_thread_start(force_tid); /* first send is 50 ms out, after CAN is started */
	return 0;
}
