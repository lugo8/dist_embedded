#include "tx.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>

#include "current.h"
#include "protocol.h"
#include "state.h"

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));

static const struct gpio_dt_spec tp_cmd_tx =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_cmd_tx_gpios);

// timing stuff
#define STATUS_PERIOD_MS 18

// serializes whole-frame sends - status_tx_thread and the link-loss test
// thread both transmit on pi_uart, and interleaved bytes from two threads
// mid-frame would corrupt both
static K_MUTEX_DEFINE(uart_tx_mutex);

// send byte by byte
void uart_send_frame(const uint8_t *frame, size_t len)
{
	k_mutex_lock(&uart_tx_mutex, K_FOREVER);
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(pi_uart, frame[i]);
	}
	k_mutex_unlock(&uart_tx_mutex);
}

static void status_tx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t seq = 0;
	int64_t next = k_uptime_get(); // absolute schedule so the ADC read + send time doesn't stretch the period

	while (1) {
		// wire field is unsigned; sensor noise around zero can read slightly negative
		int ma[CUR_COUNT];

		current_read_ma(ma);

		uint8_t data[8];
		encode_status_data(CLAMP(ma[CUR_LEFT], 0, UINT16_MAX),
				   CLAMP(ma[CUR_RIGHT], 0, UINT16_MAX),
				   CLAMP(ma[CUR_SERVO], 0, UINT16_MAX),
				   state_get_zone_state(), data);

		uint8_t frame[CMD_FRAME_LEN];
		encode_frame(MSG_STATUS, seq++, data, frame);
		uart_send_frame(frame, CMD_FRAME_LEN);
		gpio_pin_toggle_dt(&tp_cmd_tx); /* CMD_TX test point */

		next += STATUS_PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}
}

#define STATUS_TX_STACK_SIZE 1024
#define STATUS_TX_PRIORITY   6 // placeholder - revisit against the Part 4 task table

K_THREAD_DEFINE(status_tx_tid, STATUS_TX_STACK_SIZE, status_tx_thread, NULL, NULL, NULL,
		 STATUS_TX_PRIORITY, 0, SYS_FOREVER_MS);

void tx_start(void)
{
	gpio_pin_configure_dt(&tp_cmd_tx, GPIO_OUTPUT_INACTIVE);
	k_thread_start(status_tx_tid);
}
