#include "pid.h"

#include <zephyr/kernel.h>

void pid_init(struct spid *pid, float kp, float ki, float kd, float i_limit_out)
{
	pid->pGain = kp;
	pid->iGain = ki;
	pid->dGain = kd;
	pid->iMax = ki > 0.0f ? i_limit_out / ki : 0.0f;
	pid->iMin = -pid->iMax;
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

	pid->iState = CLAMP(pid->iState + error, pid->iMin, pid->iMax);
	iTerm = pid->iGain * pid->iState;

	dTerm = pid->dGain * (error - pid->dState); // calc the derivative term
	pid->dState = error;

	return pTerm + iTerm + dTerm;
}
