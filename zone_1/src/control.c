#include "control.h"

#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "blinker.h"
#include "encoder.h"
#include "motor.h"
#include "servo.h"
#include "state.h"

#include "pid.h"

#define PRINT_EVERY 20

#define RPM_AVG_N 4 

// encounder count is 4.5 rpm for a 10ms window -- take avg
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

struct ff_point {
	int rpm;
	int duty_pct;
};

static const struct ff_point ff_table[] = FF_TABLE;
#define FF_N ARRAY_SIZE(ff_table)
#define RPM_AT_DEADBAND (ff_table[0].rpm)

/* duty % expected to hold a given rpm, interpolated between measured points */
static float ff_duty(float rpm)
{
	if (rpm <= ff_table[0].rpm) {
		return ff_table[0].duty_pct;
	}

	for (size_t i = 1; i < FF_N; i++) {
		if (rpm <= ff_table[i].rpm) {
			const struct ff_point *lo = &ff_table[i - 1];
			const struct ff_point *hi = &ff_table[i];

			return lo->duty_pct + (rpm - lo->rpm) * (hi->duty_pct - lo->duty_pct) /
						      (float)(hi->rpm - lo->rpm);
		}
	}

	return ff_table[FF_N - 1].duty_pct;
}

/* Brake. A shorted-lead brake only resists in proportion to speed, so a wheel turned slowly
 * by hand still spins. Once the wheels are nearly stopped, remember where they are and drive
 * back toward that spot whenever they get pushed off it. Fast movement just gets the
 * shorted-lead brake (no plugging at speed). */
static bool hold_anchored;
static int32_t hold_l, hold_r;

static void brake_hold_release(void)
{
	hold_anchored = false;
}

static void brake_hold(uint32_t pct, float avg_rpm)
{
	int32_t pos_l, pos_r;

	encoder_position(&pos_l, &pos_r);

	if (!hold_anchored) {
		if (fabsf(avg_rpm) > HOLD_ENTER_RPM) {
			motor_brake_duty(pct);
			return;
		}
		hold_l = pos_l;
		hold_r = pos_r;
		hold_anchored = true;
	}

	int32_t err_l = hold_l - pos_l; /* > 0: pushed backward, needs to drive forward */
	int32_t err_r = hold_r - pos_r;
	int32_t big = abs(err_l) >= abs(err_r) ? err_l : err_r; /* one direction pin pair drives both */

	if (abs(big) <= HOLD_DEADBAND_COUNTS) {
		motor_brake_duty(pct);
		return;
	}

	/* enough to get the motor moving plus a bit per count of error, never more than the pedal allows */
	uint32_t d_l = MIN(pct, (uint32_t)(HOLD_MIN_DUTY_PCT + HOLD_KP_PCT_PER_COUNT * abs(err_l)));
	uint32_t d_r = MIN(pct, (uint32_t)(HOLD_MIN_DUTY_PCT + HOLD_KP_PCT_PER_COUNT * abs(err_r)));

	if (big > 0) {
		motor_forward();
	} else {
		motor_reverse();
	}
	/* only the wheels that are off in the dominant direction; the other one is left alone */
	motor_duty((err_l * big > 0) ? d_l : 0, (err_r * big > 0) ? d_r : 0);
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
	bool in_error = false; 
	bool prev_left = false;
	bool prev_right = false;
	uint32_t n = 0;
	uint32_t duty = 0;
	uint32_t srv_us = 0;
	int target_rpm = 0;
	float meas_rpm = 0.0f;
	float pid_trim = 0.0f; // last PID correction, held between ticks
	float cruise_rpm = 0.0f; // speed the car is rolling down from after the throttle is released
	int rpm_l = 0, rpm_r = 0;
	uint32_t lat_max_us = 0;
	struct spid pid;

	pid_init(&pid, PID_KP, PID_KI, PID_KD, PID_I_LIMIT_PCT, PID_I_ZONE_RPM, PID_OUT_LIMIT_PCT);

	while (1) {
		// wake on a new wheel frame, or when the next 10 ms tick is due
		bool got_frame = state_wait_wheel_frame(K_TIMEOUT_ABS_MS(next));
		bool tick = k_uptime_get() >= next;

		struct wheel_state ws;

		state_get_wheel_state(&ws);

		// rpm sampling and PID assume a fixed CONTROL_PERIOD_MS step: tick only
		if (tick) {
			next += CONTROL_PERIOD_MS;
			encoder_rpm(CONTROL_PERIOD_MS, &rpm_l, &rpm_r);
			meas_rpm = rpm_average((rpm_l + rpm_r) / 2.0f); // avg across 4 ticks
		}

		// steering keeps tracking in every state -- holds val on link loss
		// wire steering is signed (negative = left); scale +/-WIRE_AXIS_MAX onto 0..WHEEL_STEER_MAX
		int angle = CLAMP(((int)ws.steering + WIRE_AXIS_MAX) * WHEEL_STEER_MAX / (2 * WIRE_AXIS_MAX),
				  0, WHEEL_STEER_MAX);
		srv_us = servo_set_angle(angle);
		blinker_steer(angle);

		// buttons sensitive to edges
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

		uint32_t brake_pct = ((32767 - ws.brake) * 100) / 65535;
		if (error || brake_pct > 0) {
			pid_reset(&pid);
			pid_trim = 0.0f;
			target_rpm = 0;
			cruise_rpm = 0.0f;
			duty = 0;
			if (!error) {
				duty = brake_pct;
				brake_hold(brake_pct, meas_rpm);
			} else {
				brake_hold_release();
			}
		} else {
			brake_hold_release();
			// wire throttle is negative = faster; zero or positive means no throttle
			int thr = CLAMP(-(int)ws.throttle, 0, WIRE_AXIS_MAX) * WHEEL_THROTTLE_MAX / WIRE_AXIS_MAX;

			if (thr <= THROTTLE_DEADZONE) {
				/* pedal released: roll the target down like a car coasting, not hold it */
				if (tick) {
					cruise_rpm -= COAST_DECAY_RPM_PER_TICK;
				}
				cruise_rpm = MAX(cruise_rpm, 0.0f);
				target_rpm = (int)cruise_rpm;
			} else {
				/* floor at the slowest speed the motor can hold, up to max */
				target_rpm = RPM_AT_DEADBAND + (thr - THROTTLE_DEADZONE) * (MAX_TARGET_RPM - RPM_AT_DEADBAND) /
						     (WHEEL_THROTTLE_MAX - THROTTLE_DEADZONE);
				cruise_rpm = target_rpm;
			}

			if (target_rpm == 0) {
				pid_reset(&pid);
				pid_trim = 0.0f;
				duty = 0;
			} else if (target_rpm < RPM_AT_DEADBAND) {
				/* rolling off below the slowest speed the motor can hold: no rpm to track,
				 * just taper the duty down from the floor duty */
				pid_reset(&pid);
				pid_trim = 0.0f;
				duty = ff_table[0].duty_pct * target_rpm / RPM_AT_DEADBAND;
			} else {
				float ff = ff_duty(target_rpm); // est duty cycle

				// PID refines on ticks only; a frame wake reuses the last correction
				if (tick) {
					pid_trim = pid_update(&pid, target_rpm - meas_rpm);
				}
				duty = (uint32_t)(CLAMP(ff + pid_trim, 0.0f, 100.0f) + 0.5f);
			}
			motor_forward();
			motor_duty(duty, duty);
		}

		// frame -> output latency (rx store to here), worst case since last print
		if (got_frame) {
			uint32_t cyc = k_cycle_get_32() - state_wheel_frame_cycle();
			uint32_t us = (uint32_t)k_cyc_to_us_near32(cyc);

			lat_max_us = MAX(lat_max_us, us);
		}

		if (tick && ++n % PRINT_EVERY == 0) {
			printk("ctl: %-5s steer=%3d thr=%4d brk=%4d tgt=%3d avg=%3d duty=%3u%% rpm L%4d R%4d srv=%uus lat<=%uus\n",
			       error ? "ERROR" : "OK", ws.steering, ws.throttle, ws.brake,
			       target_rpm, (int)meas_rpm, duty, rpm_l, rpm_r, srv_us, lat_max_us);
			lat_max_us = 0;
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
