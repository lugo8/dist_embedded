#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include "encoder.h"

#define COUNTS_PER_REV 1320
/* tim3 is 16-bit and the driver wraps it from 65534 back to 0 */
#define RIGHT_WRAP 65535

static const struct device *enc_l = DEVICE_DT_GET(DT_NODELABEL(enc_left));
static const struct device *enc_r = DEVICE_DT_GET(DT_NODELABEL(enc_right));

static uint32_t prev_l, prev_r;

static uint32_t read_count(const struct device *dev)
{
	struct sensor_value v;

	sensor_sample_fetch(dev);
	sensor_channel_get(dev, SENSOR_CHAN_ENCODER_COUNT, &v);
	return (uint32_t)v.val1;
}

int encoder_init(void)
{
	if (!device_is_ready(enc_l) || !device_is_ready(enc_r)) {
		return -ENODEV;
	}
	prev_l = read_count(enc_l);
	prev_r = read_count(enc_r);
	return 0;
}

static int to_rpm(int32_t delta, int period_ms)
{
	return delta * 60000 / (COUNTS_PER_REV * period_ms);
}

/* call at a fixed period; returns rpm since the last call */
void encoder_rpm(int period_ms, int *left, int *right)
{
	uint32_t l = read_count(enc_l);
	uint32_t r = read_count(enc_r);

	/* tim2 is 32-bit, so unsigned subtraction handles the wrap */
	int32_t dl = (int32_t)(l - prev_l);

	/* tim3 wraps at 16 bits, so take the short way around */
	int32_t dr = (int32_t)r - (int32_t)prev_r;
	if (dr > RIGHT_WRAP / 2) {
		dr -= RIGHT_WRAP;
	} else if (dr < -RIGHT_WRAP / 2) {
		dr += RIGHT_WRAP;
	}

	prev_l = l;
	prev_r = r;

	*left = to_rpm(dl, period_ms);
	/* right motor is mirrored, so forward counts down */
	*right = -to_rpm(dr, period_ms);
}