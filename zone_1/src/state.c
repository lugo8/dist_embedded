#include "state.h"

#include <zephyr/kernel.h>

// own lock bc it's more important
static K_MUTEX_DEFINE(wheel_state_mutex);

static struct wheel_state latest_wheel_state;

static int64_t last_good_frame_ms = -(LINK_TIMEOUT_MS + 1); // for initial alive check
static uint32_t last_frame_cycle; // for latency measurement

// given on every decoded wheel frame so the control thread wakes right away
static K_SEM_DEFINE(wheel_frame_sem, 0, 1);

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
	last_frame_cycle = k_cycle_get_32();
	k_mutex_unlock(&wheel_state_mutex);

	k_sem_give(&wheel_frame_sem);
}

bool state_wait_wheel_frame(k_timeout_t timeout)
{
	return k_sem_take(&wheel_frame_sem, timeout) == 0;
}

uint32_t state_wheel_frame_cycle(void)
{
	uint32_t c;

	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	c = last_frame_cycle;
	k_mutex_unlock(&wheel_state_mutex);
	return c;
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

// power-up starts in the error state (R3)
static atomic_t zone_state = ATOMIC_INIT(ZONE_STATE_FAILSAFE_LINK_LOST);

void state_set_zone_state(uint8_t s)
{
	atomic_set(&zone_state, s);
}

uint8_t state_get_zone_state(void)
{
	return (uint8_t)atomic_get(&zone_state);
}

static int64_t last_bad_cmd_ms = -(BAD_CMD_HOLD_MS + 1); // not active at boot

void state_note_bad_cmd(void)
{
	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	last_bad_cmd_ms = k_uptime_get();
	k_mutex_unlock(&wheel_state_mutex);
}

bool bad_cmd_active(void)
{
	int64_t last;

	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	last = last_bad_cmd_ms;
	k_mutex_unlock(&wheel_state_mutex);

	return (k_uptime_get() - last) < BAD_CMD_HOLD_MS;
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
