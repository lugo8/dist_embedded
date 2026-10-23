#include "pid.h"

#include <zephyr/kernel.h>

void pid_init(struct spid *pid, float kp, float ki, float kd, float i_limit_out,
	      float i_zone, float out_limit)
{
	pid->pGain = kp;
	pid->iGain = ki;
	pid->dGain = kd;
	pid->iMax = ki > 0.0f ? i_limit_out / ki : 0.0f;
	pid->iMin = -pid->iMax;
	pid->iZone = i_zone;
	pid->outLimit = out_limit;
	pid_reset(pid);
}

void pid_reset(struct spid *pid)
{
	pid->dState = 0.0f;
	pid->iState = 0.0f;
}

float pid_update(struct spid *pid, float error)
{
	float pTerm, dTerm, iTerm;

	pTerm = pid->pGain * error;

	/* big errors (startup, hard load) would wind the integral up for nothing */
	float abs_err = error < 0.0f ? -error : error;

	if (pid->iZone <= 0.0f || abs_err <= pid->iZone) {
		pid->iState = CLAMP(pid->iState + error, pid->iMin, pid->iMax);
	}
	iTerm = pid->iGain * pid->iState;

	dTerm = pid->dGain * (error - pid->dState); // calc the derivative term
	pid->dState = error;

	float out = pTerm + iTerm + dTerm;

	if (pid->outLimit > 0.0f) {
		out = CLAMP(out, -pid->outLimit, pid->outLimit);
	}
	return out;
}
