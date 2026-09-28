#ifndef PID_H_
#define PID_H_

/* Fixed-period PID: gains are per tick, so ki and kd already include the
 * sample time. Retune if the control period changes.
 */
struct spid {
	float dState; // past error
	float iState; // integral error (sum of errors)

	float iMax; // integrator state limits (anti-windup)
	float iMin;

	float iGain; // integral gain (k_i)
	float pGain; // proportional gain (k_p)
	float dGain; // derivative gain (k_d)
};

/* i_limit_out caps the integral term's contribution to the output (same units
 * as the output), which sets iMax/iMin from ki.
 */
void pid_init(struct spid *pid, float kp, float ki, float kd, float i_limit_out);
void pid_reset(struct spid *pid);
float pid_update(struct spid *pid, float error);

#endif /* PID_H_ */
