#ifndef CONTROL_H_
#define CONTROL_H_

/* Raw ranges carried in MSG_WHEEL_STATE. PLACEHOLDERS until the real proxy
 * values are recorded (Part 1) - every conversion lives in control.c so this
 * is the only place to change.
 */
#define WHEEL_STEER_MAX    900   /* 0 = full left, 450 = center, 900 = full right */
#define WHEEL_THROTTLE_MAX 1000
#define WHEEL_BRAKE_MAX    1000
#define BRAKE_THRESHOLD    100   /* brake counts as pressed above this */

#define CONTROL_PERIOD_MS  10

/* Throttle -> velocity. PLACEHOLDERS: measure at 100% duty, free-spinning.
 * Full throttle commands MAX_TARGET_RPM (keep it below the slower wheel's top
 * speed so the loop has headroom). Rough estimate from the bench numbers:
 * 60% duty gave ~117 rpm averaged over both wheels.
 */
#define MAX_TARGET_RPM     150
#define RPM_AT_FULL_DUTY   195   /* feed-forward slope: duty% = target * 100 / this */

/* PID gains, in duty-% per rpm of error, per 10 ms tick. Start conservative. */
#define PID_KP             0.3f
#define PID_KI             0.05f
#define PID_KD             0.0f  /* encoder resolution is ~4.5 rpm/count at 10 ms; D would amplify it */
#define PID_I_LIMIT_PCT    30.0f /* max duty % the integral term may contribute */

/* Starts the control thread. Call after all peripheral inits. */
void control_start(void);

#endif /* CONTROL_H_ */
