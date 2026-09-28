/* TEMP: loopback test - alternates between bursting MSG_WHEEL_STATE frames
 * (keeps link_is_alive() true) and going silent for longer than
 * LINK_TIMEOUT_MS (forces it false). Watch the "status: ... zone_state=..."
 * prints flip between NORMAL and FAILSAFE_LINK_LOST accordingly.
 * Requires PB6 (usart1 TX) jumpered to PB7 (usart1 RX) on the Nucleo.
 * Delete this thread once the real Pi-side sender exists.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "protocol.h"
#include "tx.h"

#define LOSS_TEST_SEND_MS 2000
#define LOSS_TEST_GAP_MS  2000

static void loss_test_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t seq = 0;

	while (1) {
		printk("loss_test: sending wheel_state for %dms (expect zone_state=NORMAL)\n",
		       LOSS_TEST_SEND_MS);

		int64_t send_until = k_uptime_get() + LOSS_TEST_SEND_MS;

		while (k_uptime_get() < send_until) {
			uint8_t data[8] = {0};

			data[2] = 0x04; /* throttle = 1234 */
			data[3] = 0xD2;
			data[6] = 0x81; /* L_BTN and R_BTN both set */

			uint8_t frame[CMD_FRAME_LEN];

			encode_frame(MSG_WHEEL_STATE, seq++, data, frame);
			uart_send_frame(frame, CMD_FRAME_LEN);

			k_sleep(K_MSEC(20));
		}

		printk("loss_test: going silent for %dms (expect zone_state -> FAILSAFE_LINK_LOST)\n",
		       LOSS_TEST_GAP_MS);

		k_sleep(K_MSEC(LOSS_TEST_GAP_MS));
	}
}

#define LOSS_TEST_STACK_SIZE 1024
#define LOSS_TEST_PRIORITY   7

K_THREAD_DEFINE(loss_test_tid, LOSS_TEST_STACK_SIZE, loss_test_thread, NULL, NULL, NULL,
		 LOSS_TEST_PRIORITY, 0, 0);
