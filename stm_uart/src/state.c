#include "state.h"

#include <zephyr/kernel.h>

// own lock bc it's more important
static K_MUTEX_DEFINE(wheel_state_mutex);

static struct wheel_state latest_wheel_state;

static int64_t last_good_frame_ms = -(LINK_TIMEOUT_MS + 1); // for initial alive check

// Lower-stakes, share mutex
static K_MUTEX_DEFINE(aux_mutex);

static struct heartbeat latest_hb_rpi;
static int64_t last_rpi_hb_ms;

static struct heartbeat latest_hb_drivetrain;
static int64_t last_drivetrain_hb_ms;

static struct force_feedback latest_force_feedback;

void state_set_wheel_state(const struct wheel_state *ws)
{
	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	latest_wheel_state = *ws;
	last_good_frame_ms = k_uptime_get();
	k_mutex_unlock(&wheel_state_mutex);
}

void state_get_wheel_state(struct wheel_state *out)
{
	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	*out = latest_wheel_state;
	k_mutex_unlock(&wheel_state_mutex);
}

void state_set_hb_rpi(const struct heartbeat *hb)
{
	k_mutex_lock(&aux_mutex, K_FOREVER);
	latest_hb_rpi = *hb;
	last_rpi_hb_ms = k_uptime_get();
	k_mutex_unlock(&aux_mutex);
}

void state_set_hb_drivetrain(const struct heartbeat *hb)
{
	k_mutex_lock(&aux_mutex, K_FOREVER);
	latest_hb_drivetrain = *hb;
	last_drivetrain_hb_ms = k_uptime_get();
	k_mutex_unlock(&aux_mutex);
}

void state_set_force_feedback(const struct force_feedback *fb)
{
	k_mutex_lock(&aux_mutex, K_FOREVER);
	latest_force_feedback = *fb;
	k_mutex_unlock(&aux_mutex);
}

// check if link is alive
bool link_is_alive(void)
{
	int64_t last;

	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	last = last_good_frame_ms;
	k_mutex_unlock(&wheel_state_mutex);

	return (k_uptime_get() - last) < LINK_TIMEOUT_MS;
}
