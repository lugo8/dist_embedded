#include <zephyr/kernel.h>
#include <zephyr/drivers/adc.h>
#include "current.h"

/* acs712elc-05b */
#define MV_PER_AMP 185
/* averaging smooths out pwm ripple at the cost of a little time per read */
#define SAMPLES 8

static const struct adc_dt_spec ch[CUR_COUNT] = {
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 0),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 1),
	ADC_DT_SPEC_GET_BY_IDX(DT_PATH(zephyr_user), 2),
};

static int zero_mv[CUR_COUNT];

static int read_mv(const struct adc_dt_spec *spec)
{
	uint16_t buf;
	int32_t sum = 0;
	struct adc_sequence seq = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	adc_sequence_init_dt(spec, &seq);
	for (int i = 0; i < SAMPLES; i++) {
		adc_read_dt(spec, &seq);
		sum += buf;
	}

	int32_t mv = sum / SAMPLES;
	adc_raw_to_millivolts_dt(spec, &mv);
	return mv;
}

int current_init(void)
{
	for (int i = 0; i < CUR_COUNT; i++) {
		if (!adc_is_ready_dt(&ch[i])) {
			printk("adc not ready\n");
			return -ENODEV;
		}
		int err = adc_channel_setup_dt(&ch[i]);
		if (err) {
			printk("channel %d setup failed: %d\n", ch[i].channel_id, err);
			return err;
		}
	}

	/* each sensor sits near 2.5 v at 0 a but not exactly, so take the zero at boot while nothing is running */
	for (int i = 0; i < CUR_COUNT; i++) {
		zero_mv[i] = read_mv(&ch[i]);
	}
	printk("current zero: L %d  R %d  S %d mV\n",
	       zero_mv[CUR_LEFT], zero_mv[CUR_RIGHT], zero_mv[CUR_SERVO]);
	return 0;
}

void current_read_ma(int ma[CUR_COUNT])
{
	for (int i = 0; i < CUR_COUNT; i++) {
		ma[i] = (read_mv(&ch[i]) - zero_mv[i]) * 1000 / MV_PER_AMP;
	}
}