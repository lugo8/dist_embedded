#ifndef EDF_H_
#define EDF_H_

#include <stdint.h>

#include <zephyr/kernel.h>

#define EDF_THREAD_PRIORITY 3
#define EDF_DEADLINE_TIE_CYCLES 1
#define EDF_MAIN_DEADLINE_MS 2000

enum edf_tie_rank {
	EDF_TIE_MOTOR_CONTROL,
	EDF_TIE_CAN_RX,
	EDF_TIE_HEARTBEAT,
	EDF_TIE_BLINKER,
	EDF_TIE_MAIN,
};

static inline void edf_deadline_after_ms(k_tid_t thread, uint32_t delay_ms,
					 enum edf_tie_rank rank)
{
	int deadline = (int)k_ms_to_cyc_ceil32(delay_ms) +
		       (int)rank * EDF_DEADLINE_TIE_CYCLES;

	k_thread_deadline_set(thread, deadline);
}

static inline void edf_deadline_at_uptime_ms(k_tid_t thread, int64_t deadline_ms,
					     enum edf_tie_rank rank)
{
	int64_t delay_ms = deadline_ms - k_uptime_get();

	if (delay_ms < 0) {
		delay_ms = 0;
	}

	uint32_t deadline = k_cycle_get_32() +
			    k_ms_to_cyc_ceil32((uint32_t)delay_ms) +
			    (uint32_t)rank * EDF_DEADLINE_TIE_CYCLES;

	k_thread_absolute_deadline_set(thread, (int32_t)deadline);
}

#endif /* EDF_H_ */
