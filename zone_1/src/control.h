#ifndef CONTROL_H_
#define CONTROL_H_

#define CONTROL_PERIOD_MS  10

// TODO: placeholder values
#define WHEEL_STEER_MAX    900   /* 0 = full left, 450 = center, 900 = full right */
#define WHEEL_THROTTLE_MAX 1000
#define WHEEL_BRAKE_MAX    1000
#define BRAKE_THRESHOLD    100   /* brake counts as pressed above this */



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

// start ctrl thread
void control_start(void);

#endif /* CONTROL_H_ */
