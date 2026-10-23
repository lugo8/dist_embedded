#ifndef PID_H_
#define PID_H_

// fixed period PID -- gains are per tick
struct spid {
	float dState; // past error
	float iState; // integral error 

	// integrator state limits
	float iMax; 
	float iMin;

	float iGain; // integral gain (k_i)
	float pGain; // proportional gain (k_p)
	float dGain; // derivative gain (k_d)

	float iZone;    // integrate only when abs(error) below threshold (stops windup on big errors)
	float outLimit; // max change from pid
};

void pid_init(struct spid *pid, float kp, float ki, float kd, float i_limit_out,
	      float i_zone, float out_limit);
void pid_reset(struct spid *pid);
float pid_update(struct spid *pid, float error);

#endif /* PID_H_ */
