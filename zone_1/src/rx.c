#include "rx.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

#include "protocol.h"
#include "state.h"

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));
static const struct gpio_dt_spec tp_cmd_rx =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), tp_cmd_rx_gpios);

#define RX_RING_BUF_SIZE 64 // ring stores 64 bytes
static uint8_t rx_ring_buf_data[RX_RING_BUF_SIZE];
static struct ring_buf rx_ring_buf;

static K_SEM_DEFINE(rx_sem, 0, 1); // binary for now

// 1: print every received frame as hex (console output is slow, turn off for timing runs)
#define RX_DUMP_FRAMES 0

#if RX_DUMP_FRAMES
static void dump_frame(const char *tag, const uint8_t *frame)
{
	printk("rx %s:", tag);
	for (int i = 0; i < CMD_FRAME_LEN; i++) {
		printk(" %02x", frame[i]);
	}
	printk("\n");
}
#else
#define dump_frame(tag, frame) ARG_UNUSED(frame)
#endif

// read CMD_FRAME_LEN bytes and decode them
static void rx_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t frame[CMD_FRAME_LEN];

	while (1) {
		k_sem_take(&rx_sem, K_FOREVER);

		while (ring_buf_size_get(&rx_ring_buf) >= CMD_FRAME_LEN) {
			uint8_t msg_type;

			ring_buf_peek(&rx_ring_buf, &msg_type, 1);

			// unidentifiable msg -- keep looking
			if (!is_known_msg_type(msg_type)) {
				ring_buf_get(&rx_ring_buf, NULL, 1);
				continue;
			}

			ring_buf_get(&rx_ring_buf, frame, CMD_FRAME_LEN);

			// if checksum fails, discard
			if (checksum(frame, CMD_CHK_LEN) != frame[CMD_FRAME_LEN - 1]) {
				printk("rx_thread: bad checksum (type 0x%02x)\n", msg_type);
				dump_frame("BAD", frame);
				continue;
			}

			switch (msg_type) {
				case MSG_WHEEL_STATE: {
					struct wheel_state ws;

					// check out of range (btns only)
					if (!wheel_frame_valid(frame)) {
						state_note_bad_cmd();
						break;
					}

					decode_wheel_state(frame, &ws);
					gpio_pin_toggle_dt(&tp_cmd_rx); /* CMD_RX test point */
					state_set_wheel_state(&ws);
					break;
				}
				case MSG_HB_RPI: {
					struct heartbeat hb;

					decode_heartbeat(frame, &hb);
					state_set_hb_rpi(&hb);
					break;
				}
				case MSG_HB_DRIVETRAIN: {
					struct heartbeat hb;

					decode_heartbeat(frame, &hb);
					state_set_hb_drivetrain(&hb);
					break;
				}
				case MSG_FORCE_FDBK: {
					struct force_feedback fb;

					decode_force_feedback(frame, &fb);
					state_set_force_feedback(&fb);
					break;
				}
				case MSG_STATUS: {
					struct status_frame sf;

					decode_status(frame, &sf);

					/* TEMP: loopback test - dump decoded STATUS fields, once a second */
					if (sf.seq % 50 == 0) {
						printk("status: seq=%u motor_a=%u motor_b=%u servo=%u zone_state=%s\n",
						       sf.seq, sf.motor_a_current, sf.motor_b_current,
						       sf.servo_current, zone_state_name(sf.zone_state));
					}
					break;
				}
				default: {
					printk("WRONG MSG\n");
				}
			}

			// after the switch so the store + control wake-up aren't delayed by the print
			dump_frame("ok ", frame);
		}
	}
}

#define RX_STACK_SIZE 1024
#define RX_PRIORITY 5 // TODO: placeholder

K_THREAD_DEFINE(rx_tid, RX_STACK_SIZE, rx_thread, NULL, NULL, NULL,
		        RX_PRIORITY, 0, 0);

static void pi_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	// uart_irq_update saves
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t byte;

		// read from hw buffer
		if (uart_fifo_read(dev, &byte, 1) != 1) {
			break;
		}

		// put what we read into our sw buffer
		ring_buf_put(&rx_ring_buf, &byte, 1);
	}

	// wake up parsing thread
	k_sem_give(&rx_sem);
}

void rx_init(void)
{
	gpio_pin_configure_dt(&tp_cmd_rx, GPIO_OUTPUT_INACTIVE);
	ring_buf_init(&rx_ring_buf, sizeof(rx_ring_buf_data), rx_ring_buf_data);
	uart_irq_callback_user_data_set(pi_uart, pi_uart_isr, NULL);
	uart_irq_rx_enable(pi_uart);
}
