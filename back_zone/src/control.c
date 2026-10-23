#include "control.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>

#include "encoder.h"
#include "io_status.h"
#include "motor.h"
#include "pid.h"
#include "protocol.h"
#include "state.h"

#define RPM_AVG_N 4
#define ENCODER_STALL_TIMEOUT_MS 1000

struct ff_point {
	int rpm;
	int duty_pct;
};

static const struct ff_point ff_table[] = FF_TABLE;
#define FF_N ARRAY_SIZE(ff_table)
#define RPM_AT_DEADBAND (ff_table[0].rpm)

static bool encoder_feedback_available;

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

static float ff_duty(float rpm)
{
	if (rpm <= ff_table[0].rpm) {
		return ff_table[0].duty_pct;
	}

	for (size_t i = 1; i < FF_N; i++) {
		if (rpm <= ff_table[i].rpm) {
			const struct ff_point *lo = &ff_table[i - 1];
			const struct ff_point *hi = &ff_table[i];

			return lo->duty_pct +
			       (rpm - lo->rpm) * (hi->duty_pct - lo->duty_pct) /
			       (float)(hi->rpm - lo->rpm);
		}
	}

	return ff_table[FF_N - 1].duty_pct;
}

static int throttle_duty(int16_t throttle)
{
	int throttle_scaled =
		CLAMP(-(int)throttle, 0, WIRE_AXIS_MAX) *
		WHEEL_THROTTLE_MAX / WIRE_AXIS_MAX;

	if (throttle_scaled <= THROTTLE_DEADZONE) {
		return 0;
	}

	int target_rpm =
		RPM_AT_DEADBAND +
		(throttle_scaled - THROTTLE_DEADZONE) *
			(MAX_TARGET_RPM - RPM_AT_DEADBAND) /
			(WHEEL_THROTTLE_MAX - THROTTLE_DEADZONE);

	return (int)(ff_duty(target_rpm) + 0.5f);
}

int control_apply_zone_control(const struct wheel_state *ws)
{
	printk("control_apply_zone_control: steering=%d throttle=%d brake=%d left_btn=%d right_btn=%d self_test_btn=%d btn_a=%d btn_y=%d\n",
	       ws->steering, ws->throttle, ws->brake, ws->left_btn, ws->right_btn,
	       ws->self_test_btn, ws->btn_a, ws->btn_y);
	if (!ws) {
		return -EINVAL;
	}

	bool error = io_status_error_active() || !link_is_alive() ||
		     bad_cmd_active();
	uint32_t brake_pct =
		(uint32_t)(INT16_MAX - (int32_t)ws->brake) * 100U /
		UINT16_MAX;

	if (error) {
		printk("control_apply_zone_control: error state active; braking\n");
		return motor_brake_duty(100);
	}
	if (brake_pct > 0U) {
		printk("control_apply_zone_control: brake active (%u%%); braking\n", brake_pct);
		return motor_brake_duty(brake_pct);
	}

	int duty = throttle_duty(ws->throttle);
	int rc = motor_forward();
	printk("control_apply_zone_control: throttle=%d duty=%d%%\n", ws->throttle, duty);

	return rc ? rc : motor_duty(duty);
}

static struct spid hold_pid_l;
static struct spid hold_pid_r;
static float trim_l;
static float trim_r;
static bool hold_anchored;
static bool hold_driving;
static bool hold_pushed;
static int hold_dir;
static int64_t hold_drive_ms;
static int32_t hold_l;
static int32_t hold_r;
static int32_t hold_prev_abs;

static void brake_hold_reset_pid(void)
{
	pid_reset(&hold_pid_l);
	pid_reset(&hold_pid_r);
	trim_l = 0.0f;
	trim_r = 0.0f;
}

static void brake_hold_release(void)
{
	hold_anchored = false;
	hold_driving = false;
	hold_dir = 0;
}

static void apply_brake(uint32_t pct, float avg_rpm, bool tick)
{
	if (!encoder_feedback_available) {
		if (motor_brake_duty(pct)) {
			io_status_mark_error();
		}
		return;
	}

	int32_t pos_l;
	int32_t pos_r;

	encoder_position(&pos_l, &pos_r);

	if (!hold_anchored) {
		if (fabsf(avg_rpm) > HOLD_ENTER_RPM) {
			if (motor_brake_duty(pct)) {
				io_status_mark_error();
			}
			return;
		}
		hold_l = pos_l;
		hold_r = pos_r;
		hold_anchored = true;
		hold_prev_abs = 0;
		hold_pushed = false;
		brake_hold_reset_pid();
	}

	int32_t err_l = hold_l - pos_l;
	int32_t err_r = hold_r - pos_r;
	int32_t big = abs(err_l) >= abs(err_r) ? err_l : err_r;

	if (tick) {
		hold_pushed = abs(big) < hold_prev_abs;
		hold_prev_abs = abs(big);
	}

	if (hold_driving) {
		hold_driving = abs(big) > HOLD_EXIT_COUNTS;
	} else {
		hold_driving = abs(big) > HOLD_ENTER_COUNTS;
	}

	int dir = big > 0 ? 1 : -1;
	int64_t now = k_uptime_get();
	bool flipping = hold_dir != 0 && dir != hold_dir &&
			(now - hold_drive_ms) < HOLD_REVERSE_LOCKOUT_MS;

	if (!hold_driving || flipping || hold_pushed) {
		if (!hold_driving) {
			brake_hold_reset_pid();
		}
		if (motor_brake_duty(pct)) {
			io_status_mark_error();
		}
		return;
	}

	hold_dir = dir;
	hold_drive_ms = now;

	bool drive_l = err_l * big > 0;
	bool drive_r = err_r * big > 0;

	if (tick) {
		trim_l = drive_l ? pid_update(&hold_pid_l, abs(err_l)) : 0.0f;
		trim_r = drive_r ? pid_update(&hold_pid_r, abs(err_r)) : 0.0f;
		if (!drive_l) {
			pid_reset(&hold_pid_l);
		}
		if (!drive_r) {
			pid_reset(&hold_pid_r);
		}
	}

	uint32_t duty_l = (uint32_t)CLAMP(HOLD_MIN_DUTY_PCT + trim_l,
					  0.0f, (float)pct);
	uint32_t duty_r = (uint32_t)CLAMP(HOLD_MIN_DUTY_PCT + trim_r,
					  0.0f, (float)pct);
	uint32_t duty = MAX(drive_l ? duty_l : 0U,
			    drive_r ? duty_r : 0U);
	int rc = dir > 0 ? motor_forward() : motor_reverse();

	if (!rc) {
		rc = motor_duty(duty);
	}
	if (rc) {
		io_status_mark_error();
	}
}

static void control_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct spid speed_pid;
	pid_init(&speed_pid, PID_KP, PID_KI, PID_KD, PID_I_LIMIT_PCT,
		 PID_I_ZONE_RPM, PID_OUT_LIMIT_PCT);
	pid_init(&hold_pid_l, HOLD_KP, HOLD_KI, HOLD_KD, HOLD_I_LIMIT_PCT,
		 HOLD_I_ZONE_COUNTS, HOLD_OUT_LIMIT_PCT);
	pid_init(&hold_pid_r, HOLD_KP, HOLD_KI, HOLD_KD, HOLD_I_LIMIT_PCT,
		 HOLD_I_ZONE_COUNTS, HOLD_OUT_LIMIT_PCT);

	int64_t next = k_uptime_get();
	int64_t encoder_motion_ms[2] = { next, next };
	int32_t previous_position[2] = { 0, 0 };
	bool command_active = false;
	uint8_t log_counter = 0;
	int target_rpm = 0;
	int rpm_l = 0;
	int rpm_r = 0;
	float measured_rpm = 0.0f;
	float pid_trim = 0.0f;
	float cruise_rpm = 0.0f;
	bool feedback_error_reported = !encoder_feedback_available;

	while (1) {
		bool got_frame = state_wait_wheel_frame(K_TIMEOUT_ABS_MS(next));
		bool tick = k_uptime_get() >= next;
		int64_t now = k_uptime_get();
		struct wheel_state ws;

		state_get_wheel_state(&ws);

		if (tick) {
			next += CONTROL_PERIOD_MS;
			if (encoder_feedback_available) {
				if (encoder_rpm(CONTROL_PERIOD_MS, &rpm_l, &rpm_r)) {
					encoder_feedback_available = false;
				} else {
					measured_rpm = rpm_average((rpm_l + rpm_r) / 2.0f);
				}
			}
		}

		bool error = io_status_error_active() || !link_is_alive() ||
			     bad_cmd_active();
		uint32_t brake_pct =
			(uint32_t)(INT16_MAX - (int32_t)ws.brake) * 100U /
			UINT16_MAX;

		if (error || brake_pct > 0U) {
			pid_reset(&speed_pid);
			pid_trim = 0.0f;
			target_rpm = 0;
			cruise_rpm = 0.0f;
			command_active = false;
			if (!hold_anchored) {
				hold_prev_abs = 0;
				hold_pushed = false;
				hold_driving = false;
			}
			apply_brake(error ? 100U : brake_pct, measured_rpm, tick);
		} else {
			brake_hold_release();
			int throttle =
				CLAMP(-(int)ws.throttle, 0, WIRE_AXIS_MAX) *
				WHEEL_THROTTLE_MAX / WIRE_AXIS_MAX;

			if (throttle <= THROTTLE_DEADZONE) {
				cruise_rpm = 0.0f;
				target_rpm = 0;
			} else {
				target_rpm =
					RPM_AT_DEADBAND +
					(throttle - THROTTLE_DEADZONE) *
						(MAX_TARGET_RPM - RPM_AT_DEADBAND) /
						(WHEEL_THROTTLE_MAX -
						 THROTTLE_DEADZONE);
				cruise_rpm = target_rpm;
			}

			if (encoder_feedback_available && tick && target_rpm > 0) {
				int32_t pos_l;
				int32_t pos_r;

				encoder_position(&pos_l, &pos_r);
				if (!command_active) {
					previous_position[0] = pos_l;
					previous_position[1] = pos_r;
					encoder_motion_ms[0] = now;
					encoder_motion_ms[1] = now;
					command_active = true;
				} else {
					if (pos_l != previous_position[0]) {
						previous_position[0] = pos_l;
						encoder_motion_ms[0] = now;
					}
					if (pos_r != previous_position[1]) {
						previous_position[1] = pos_r;
						encoder_motion_ms[1] = now;
					}
					if (now - encoder_motion_ms[0] >=
						    ENCODER_STALL_TIMEOUT_MS ||
					    now - encoder_motion_ms[1] >=
						    ENCODER_STALL_TIMEOUT_MS) {
						encoder_feedback_available = false;
						pid_reset(&speed_pid);
						pid_trim = 0.0f;
						printk("Encoder feedback unavailable; using feed-forward motor control\n");
					}
				}
			} else if (target_rpm == 0) {
				command_active = false;
			}

			uint32_t duty = 0;

			if (target_rpm == 0) {
				pid_reset(&speed_pid);
				pid_trim = 0.0f;
			} else if (target_rpm < RPM_AT_DEADBAND) {
				pid_reset(&speed_pid);
				pid_trim = 0.0f;
				duty = ff_table[0].duty_pct * target_rpm /
				       RPM_AT_DEADBAND;
			} else {
				float ff = ff_duty(target_rpm);

				if (encoder_feedback_available && tick) {
					pid_trim = pid_update(&speed_pid,
							      target_rpm - measured_rpm);
				}
				duty = (uint32_t)(CLAMP(
					ff + (encoder_feedback_available ? pid_trim : 0.0f),
					0.0f, 100.0f) + 0.5f);
			}

			int rc = motor_forward();

			if (!rc) {
				rc = motor_duty(duty);
			}
			if (rc) {
				printk("Motor output update failed: %d\n", rc);
				io_status_mark_error();
			}
		}

		if (!encoder_feedback_available && !feedback_error_reported) {
			printk("Encoder feedback unavailable; using feed-forward motor control\n");
			feedback_error_reported = true;
		}

		if (got_frame && tick && ++log_counter >= 100) {
			log_counter = 0;
			printk("control: %s throttle=%d brake=%d target=%d rpm=%d duty feedback=%s\n",
			       error ? "BRAKE" : "DRIVE", ws.throttle, ws.brake,
			       target_rpm, (int)measured_rpm,
			       encoder_feedback_available ? "PID" : "off");
		}
	}
}

#define CONTROL_STACK_SIZE 1536
#define CONTROL_PRIORITY 3

K_THREAD_DEFINE(control_tid, CONTROL_STACK_SIZE, control_thread,
		NULL, NULL, NULL, CONTROL_PRIORITY, 0, SYS_FOREVER_MS);

void control_start(bool encoder_available)
{
	encoder_feedback_available = encoder_available;
	k_thread_start(control_tid);
}
