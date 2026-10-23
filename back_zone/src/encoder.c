#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <errno.h>
#include "encoder.h"

#define COUNTS_PER_REV 1320
/* tim3 is 16-bit and the driver wraps it from 65534 back to 0 */
#define RIGHT_WRAP 65535

static const struct device *enc_l = DEVICE_DT_GET(DT_NODELABEL(enc_left));
static const struct device *enc_r = DEVICE_DT_GET(DT_NODELABEL(enc_right));

static uint32_t prev_l, prev_r;
static int32_t pos_l, pos_r;

static int read_count(const struct device *dev, uint32_t *count)
{
	struct sensor_value v;
	int rc = sensor_sample_fetch(dev);

	if (rc) {
		return rc;
	}
	rc = sensor_channel_get(dev, SENSOR_CHAN_ENCODER_COUNT, &v);
	if (rc) {
		return rc;
	}

	*count = (uint32_t)v.val1;
	return 0;
}

int encoder_init(void)
{
	if (!device_is_ready(enc_l) || !device_is_ready(enc_r)) {
		return -ENODEV;
	}
	int rc = read_count(enc_l, &prev_l);

	if (rc) {
		return rc;
	}
	rc = read_count(enc_r, &prev_r);
	if (rc) {
		return rc;
	}

	pos_l = 0;
	pos_r = 0;
	return rc;
}

static int to_rpm(int32_t delta, int period_ms)
{
	return delta * 60000 / (COUNTS_PER_REV * period_ms);
}

/* call at a fixed period; returns rpm since the last call */
int encoder_rpm(int period_ms, int *left, int *right)
{
	if (period_ms <= 0) {
		return -EINVAL;
	}

	uint32_t l;
	uint32_t r;
	int rc = read_count(enc_l, &l);

	if (rc) {
		return rc;
	}
	rc = read_count(enc_r, &r);
	if (rc) {
		return rc;
	}

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
	pos_l += dl;
	pos_r -= dr; /* mirrored, see below */

	*left = to_rpm(dl, period_ms);
	/* right motor is mirrored, so forward counts down */
	*right = -to_rpm(dr, period_ms);
	return 0;
}

void encoder_position(int32_t *left, int32_t *right)
{
	*left = pos_l;
	*right = pos_r;
}
