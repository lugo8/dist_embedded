/* HB_TX, HB_MON (liveness, degraded LEDs, error state, watchdog) and TEST_BTN */
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include "zone.h"

#define USER DT_PATH(zephyr_user)

/* N3 has no ERR test point, so the pin is optional */
static const struct gpio_dt_spec err_pin = GPIO_DT_SPEC_GET_OR(USER, err_gpios, {0});
static const struct gpio_dt_spec deg_pin[2] = {
	GPIO_DT_SPEC_GET(USER, deg_a_gpios),
	GPIO_DT_SPEC_GET(USER, deg_b_gpios),
};
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct device *const wdt = DEVICE_DT_GET(DT_NODELABEL(iwdg));

/* shared with the dispatch path and the actuator tasks; times in ms, 0 = never */
static K_MUTEX_DEFINE(lock);
static int64_t hb_ms[4][2]; /* [zone][bus] last heartbeat */
static int64_t any_ms[2];   /* [bus] last frame from any other zone */
static int64_t ctrl_ms;     /* last valid CTRL */
static int64_t bad_cmd_ms;  /* last out-of-range CTRL */
static bool ctrl_since_error;
static bool in_error = true; /* power up in error (R3) */
static bool self_test;
static bool deg[2];

static int wdt_channel;
static atomic_t fault_seq;

static void set_err(int v)
{
	if (err_pin.port) {
		gpio_pin_set_dt(&err_pin, v);
	}
}

static bool recent(int64_t t, int64_t now, int64_t window)
{
	return t > 0 && now - t < window;
}

static void enter_error(int zone, int reason)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool was = in_error;

	in_error = true;
	ctrl_since_error = false;
	k_mutex_unlock(&lock);
	if (was) {
		return;
	}

	set_err(1);
	actuator_error();
	uint8_t d[MSG_DLC] = { 0, zone, reason, MY_ZONE };
	send_both(ID_FAULT, &fault_seq, d);
}

void monitor_rx(int bus, int zone, bool hb)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&lock, K_FOREVER);
	any_ms[bus] = now;
	if (hb) {
		hb_ms[zone][bus] = now;
	}
	k_mutex_unlock(&lock);
}

void monitor_ctrl(bool valid)
{
	int64_t now = k_uptime_get();

	k_mutex_lock(&lock, K_FOREVER);
	if (valid) {
		ctrl_ms = now;
		ctrl_since_error = true;
	} else {
		bad_cmd_ms = now;
	}
	k_mutex_unlock(&lock);

	if (!valid) {
		enter_error(MY_ZONE, FAULT_BAD_CTRL);
	}
}

bool monitor_in_error(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool e = in_error;
	k_mutex_unlock(&lock);
	return e;
}

bool monitor_hb_on_bus(int zone, int bus)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool on = recent(hb_ms[zone][bus], k_uptime_get(), RELAY_DETECT_MS);
	k_mutex_unlock(&lock);
	return on;
}

bool monitor_bus_degraded(int bus)
{
	k_mutex_lock(&lock, K_FOREVER);
	bool d = deg[bus];
	k_mutex_unlock(&lock);
	return d;
}

/* HB_TX: 20 ms */
static void hb_tx_task(void *p1, void *p2, void *p3)
{
	static atomic_t seq;
	int64_t next = k_uptime_get();

	while (1) {
		k_mutex_lock(&lock, K_FOREVER);
		bool quiet = self_test;
		uint8_t d[MSG_DLC] = { 0, in_error ? STATE_ERROR : STATE_NORMAL,
				       (deg[0] ? HEALTH_A_DEG : 0) | (deg[1] ? HEALTH_B_DEG : 0) };
		k_mutex_unlock(&lock);

		if (!quiet) {
			send_both(MY_HB, &seq, d); /* silence is how the others see the test button */
		}
		next += HB_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

/* HB_MON: liveness of the other zones per bus, degraded LEDs, error state */
static void hb_mon_task(void *p1, void *p2, void *p3)
{
	int64_t next = k_uptime_get();

	while (1) {
		int64_t now = k_uptime_get();

		k_mutex_lock(&lock, K_FOREVER);
		bool n1 = recent(hb_ms[ZONE_N1][0], now, HB_TIMEOUT_MS) ||
			  recent(hb_ms[ZONE_N1][1], now, HB_TIMEOUT_MS);
		bool peer = recent(hb_ms[PEER_ZONE][0], now, HB_TIMEOUT_MS) ||
			    recent(hb_ms[PEER_ZONE][1], now, HB_TIMEOUT_MS);
		bool ctrl_ok = recent(ctrl_ms, now, CTRL_TIMEOUT_MS);
		bool bad_hold = recent(bad_cmd_ms, now, BAD_CMD_HOLD_MS);
		bool ok = n1 && peer && ctrl_ok && !self_test && !bad_hold;

		for (int b = 0; b < 2; b++) {
			deg[b] = !recent(any_ms[b], now, DEG_TIMEOUT_MS);
		}
		/* out of error only with both other zones heard and a fresh valid CTRL (R3) */
		bool leave = in_error && ok && ctrl_since_error;
		bool enter = !in_error && !ok;

		if (leave) {
			in_error = false;
		}
		bool d0 = deg[0], d1 = deg[1];
		k_mutex_unlock(&lock);

		gpio_pin_set_dt(&deg_pin[0], d0);
		gpio_pin_set_dt(&deg_pin[1], d1);
		if (leave) {
			set_err(0);
		}
		if (enter) {
			enter_error(!peer ? PEER_ZONE : ZONE_N1,
				    (n1 && peer) ? FAULT_CTRL_LOST : FAULT_HB_LOST);
		}
		wdt_feed(wdt, wdt_channel);

		next += MON_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

/* TEST_BTN: the ISR only wakes the task; single press fails the zone, double press clears */
static K_SEM_DEFINE(btn_sem, 0, 1);
static struct gpio_callback btn_cb;
static int64_t btn_edge_ms;

static void btn_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	int64_t now = k_uptime_get();

	/* the first edge counts right away; bounces right after it don't */
	if (now - btn_edge_ms >= DEBOUNCE_MS) {
		k_sem_give(&btn_sem);
	}
	btn_edge_ms = now;
}

static void btn_task(void *p1, void *p2, void *p3)
{
	int64_t last_press = 0;

	while (1) {
		k_sem_take(&btn_sem, K_FOREVER);
		int64_t now = k_uptime_get();

		k_mutex_lock(&lock, K_FOREVER);
		bool clear = self_test && now - last_press <= DOUBLE_PRESS_MS;

		self_test = !clear;
		k_mutex_unlock(&lock);

		if (clear) {
			last_press = 0; /* HB_MON decides when we leave error */
		} else {
			last_press = now;
			enter_error(MY_ZONE, FAULT_TEST_BTN);
		}
	}
}

K_THREAD_DEFINE(hb_tx_tid, 1024, hb_tx_task, NULL, NULL, NULL, PRIO_HB, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(hb_mon_tid, 1024, hb_mon_task, NULL, NULL, NULL, PRIO_HB, 0, SYS_FOREVER_MS);
K_THREAD_DEFINE(btn_tid, 1024, btn_task, NULL, NULL, NULL, PRIO_BTN, 0, SYS_FOREVER_MS);

int monitor_init(void)
{
	if (err_pin.port && gpio_pin_configure_dt(&err_pin, GPIO_OUTPUT_ACTIVE)) {
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&button) || !device_is_ready(wdt) ||
	    gpio_pin_configure_dt(&deg_pin[0], GPIO_OUTPUT_INACTIVE) ||
	    gpio_pin_configure_dt(&deg_pin[1], GPIO_OUTPUT_INACTIVE) ||
	    gpio_pin_configure_dt(&button, GPIO_INPUT)) {
		return -ENODEV;
	}
	gpio_init_callback(&btn_cb, btn_isr, BIT(button.pin));
	gpio_add_callback(button.port, &btn_cb);
	gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);

	struct wdt_timeout_cfg cfg = {
		.window = { .min = 0, .max = WDT_TIMEOUT_MS },
		.flags = WDT_FLAG_RESET_SOC,
	};
	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0 || wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG)) {
		return -EIO;
	}

	k_thread_start(hb_tx_tid);
	k_thread_start(hb_mon_tid);
	k_thread_start(btn_tid);
	return 0;
}
