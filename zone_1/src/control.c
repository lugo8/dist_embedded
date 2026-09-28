/* Consumes the latest wheel state (filled by rx_thread) and drives the
 * actuators. Fixed 10 ms period so encoder_rpm() sees a constant window.
 */
#include "control.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "encoder.h"
#include "motor.h"
#include "servo.h"
#include "state.h"

#include "pid.h"

#define PRINT_EVERY 20 /* one status line per 200 ms */

/* One encoder count is ~4.5 rpm over a 10 ms window, so smooth over 4 ticks */
#define RPM_AVG_N 4

static float rpm_average(float sample)
{
	static float buf[RPM_AVG_N];
	static float sum;
	static int idx;

	sum += sample - buf[idx];
	buf[idx] = sample;
	idx = (idx + 1) % RPM_AVG_N;
	return sum / RPM_AVG_N;
}

static void enter_error(void)
{
	motor_brake();
	blinker_hazard(true);
	state_set_zone_state(ZONE_STATE_FAILSAFE_LINK_LOST);
	printk("ctl: ERROR (link lost) -> brake + hazards\n");
}

static void leave_error(void)
{
	blinker_hazard(false);
	state_set_zone_state(ZONE_STATE_NORMAL);
	printk("ctl: link back -> normal\n");
}

static void control_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int64_t next = k_uptime_get();
	bool in_error = false; /* false so the first tick runs enter_error() */
	bool prev_left = false, prev_right = false;
	uint32_t n = 0;
	uint32_t duty = 0;
	uint32_t srv_us = 0;
	int target_rpm = 0;
	float meas_rpm = 0.0f;
	struct spid pid;

	pid_init(&pid, PID_KP, PID_KI, PID_KD, PID_I_LIMIT_PCT);

	while (1) {
		next += CONTROL_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));

		struct wheel_state ws;
		int rpm_l, rpm_r;

		state_get_wheel_state(&ws);
		encoder_rpm(CONTROL_PERIOD_MS, &rpm_l, &rpm_r);
		/* keep the window running in every state so it is current on exit */
		meas_rpm = rpm_average((rpm_l + rpm_r) / 2.0f);

		/* steering keeps tracking in every state (holds last value on link loss) */
		int angle = CLAMP(ws.steering, 0, WHEEL_STEER_MAX);

		srv_us = servo_set_angle(angle);
		blinker_steer(angle);

		/* buttons: act on the press edge, the wheel holds them as a level */
		if (ws.left_btn && !prev_left) {
			blinker_left();
		}
		if (ws.right_btn && !prev_right) {
			blinker_right();
		}
		prev_left = ws.left_btn;
		prev_right = ws.right_btn;

		bool error = !link_is_alive();

		if (error != in_error) {
			in_error = error;
			if (error) {
				enter_error();
			} else {
				leave_error();
			}
		}

		if (error || ws.brake > BRAKE_THRESHOLD) {
			/* don't let the integrator wind up while we aren't driving */
			pid_reset(&pid);
			target_rpm = 0;
			duty = 0;
			if (!error) {
				/* brake beats throttle, always */
				motor_brake();
			}
		} else {
			target_rpm = CLAMP(ws.throttle, 0, WHEEL_THROTTLE_MAX) * MAX_TARGET_RPM /
				     WHEEL_THROTTLE_MAX;

			if (target_rpm == 0) {
				pid_reset(&pid);
				duty = 0;
			} else {
				/* feed-forward puts the duty near right immediately; PID trims it */
				float ff = target_rpm * 100.0f / RPM_AT_FULL_DUTY;
				float out = ff + pid_update(&pid, target_rpm - meas_rpm);

				duty = (uint32_t)(CLAMP(out, 0.0f, 100.0f) + 0.5f);
			}
			motor_forward();
			motor_duty(duty, duty);
		}

		if (++n % PRINT_EVERY == 0) {
			printk("ctl: %-5s steer=%3d thr=%4d brk=%4d tgt=%3d avg=%3d duty=%3u%% rpm L%4d R%4d srv=%uus\n",
			       error ? "ERROR" : "OK", ws.steering, ws.throttle, ws.brake,
			       target_rpm, (int)meas_rpm, duty, rpm_l, rpm_r, srv_us);
		}
	}
}

#define CONTROL_STACK_SIZE 1536
#define CONTROL_PRIORITY   3 /* above rx (5) and status tx (6): brake has the tightest deadline */

K_THREAD_DEFINE(control_tid, CONTROL_STACK_SIZE, control_thread, NULL, NULL, NULL,
		CONTROL_PRIORITY, 0, SYS_FOREVER_MS);

void control_start(void)
{
	k_thread_start(control_tid);
}
