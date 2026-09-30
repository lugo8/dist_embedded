#ifndef CONTROL_H_
#define CONTROL_H_

#define CONTROL_PERIOD_MS  10

// TODO: placeholder values
#define WHEEL_STEER_MAX    900   /* servo/blinker scale: 0 = full left, 450 = center, 900 = full right */
#define STEER_CENTER       (WHEEL_STEER_MAX / 2)
/* the Pi forwards the raw wheel axes (lX/lY/lRz) as signed 16-bit values:
 * steering negative = left, 0 = center; throttle/brake negative = pressed.
 * Full travel is +/-WIRE_AXIS_MAX, scaled onto the 0..WHEEL_*_MAX scales below. */
#define WIRE_AXIS_MAX      32767
#define WHEEL_THROTTLE_MAX 1000  /* magnitude; on the wire throttle is negative = faster */
#define WHEEL_BRAKE_MAX    1000

/* brake hold: once nearly stopped, lock the wheels at the position they stopped at */
#define HOLD_ENTER_RPM        5    /* above this just short the leads; anchor only once basically stopped
                                    * (1 count per 10 ms tick is ~4.5 rpm). Anchoring while still rolling
                                    * makes the wheels overshoot the anchor and the 55% kick back jitters */
/* hysteresis: start driving back once the error passes ENTER, stop (and just short the leads) once it
 * is inside EXIT. A single 55% push moves the wheel further than a small deadband, so a tight
 * deadband makes it overshoot and kick back and forth. 1320 counts/rev. */
#define HOLD_ENTER_COUNTS     8    /* ~2 deg of wheel travel; raise if it chatters at rest */
#define HOLD_EXIT_COUNTS      3
#define HOLD_REVERSE_LOCKOUT_MS 50 /* no direction flip this soon after driving the other way */
#define HOLD_MIN_DUTY_PCT     55   /* the motor doesn't move below ~50% duty, so push at least this hard */
/* hold PID: duty = HOLD_MIN_DUTY_PCT (feedforward) + PID(position error in counts). Gains are per 10 ms tick. */
#define HOLD_KP               0.5f /* duty % per encoder count of error */
#define HOLD_KI               0.0f /* off: the floor duty already overshoots, integral only adds push */
#define HOLD_KD               0.0f /* off until the limit cycle is gone; error changes ~30 counts/tick at speed */
#define HOLD_I_LIMIT_PCT      15.0f /* max duty % the integral term can contribute */
#define HOLD_I_ZONE_COUNTS    30.0f /* only integrate when the error is within this many counts */
#define HOLD_OUT_LIMIT_PCT    20.0f /* max PID trim either way */

/* throttle released: target rpm falls by this every 10 ms tick (0.5 -> 230 rpm rolls off in ~4.6 s;
 * below the slowest holdable rpm the duty tapers to 0 over the rest of the ramp) */
#define COAST_DECAY_RPM_PER_TICK 0.5f

/* PID */
#define MAX_TARGET_RPM     230

/* feedforward table */
#define FF_TABLE { \
	{ 143,  60 }, \
	{ 208,  70 }, \
	{ 266,  80 }, \
	{ 302,  90 }, \
	{ 315, 100 }, \
}
/* does not move <=50% duty; throttle at or below this commands 0 rpm */
#define THROTTLE_DEADZONE  50

// gains
#define PID_KP             0.2f
#define PID_KI             0.01f
#define PID_KD             0.0f
#define PID_I_LIMIT_PCT    30.0f // max duty % the integral term can contribute
#define PID_I_ZONE_RPM     30.0f // only integrate when abs(error) is within this
#define PID_OUT_LIMIT_PCT  20.0f // max duty the PID may change

// self test
#define SELF_TEST_DEBOUNCE_MS     20
#define SELF_TEST_DOUBLE_PRESS_MS 500

// start ctrl thread
void control_start(void);

#endif /* CONTROL_H_ */

