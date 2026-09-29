#include "inject.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "control.h"
#include "protocol.h"
#include "tx.h"

#define SEND_PERIOD_MS 20
#define BTN_HOLD_TICKS 5 /* a "press" lasts 100 ms, like a real button */

static const struct device *const con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));

static void print_help(void)
{
	printk("\nsteer:    a/d -/+50   c center\n");
	printk("throttle: w/s +/-100  x zero\n");
	printk("brake:    space toggle\n");
	printk("blinker:  q left  e right (button press)\n");
	printk("link:     l cut/restore the frame stream (link-loss test)\n");
	printk("other:    ? help\n\n");
}

static void inject_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int steering = WHEEL_STEER_MAX / 2;
	int throttle = 0;
	int brake = 0;
	int left_hold = 0, right_hold = 0;
	bool link_cut = false;
	uint8_t seq = 0;
	unsigned char c;

	print_help();

	while (1) {
		while (uart_poll_in(con, &c) == 0) {
			switch (c) {
			case 'a':
				steering = CLAMP(steering - 50, 0, WHEEL_STEER_MAX);
				break;
			case 'd':
				steering = CLAMP(steering + 50, 0, WHEEL_STEER_MAX);
				break;
			case 'c':
				steering = WHEEL_STEER_MAX / 2;
				break;
			case 'w':
				throttle = MIN(throttle + 100, WHEEL_THROTTLE_MAX);
				break;
			case 's':
				throttle = MAX(throttle - 100, 0);
				break;
			case 'x':
				throttle = 0;
				break;
			case ' ':
				brake = brake ? 0 : -WIRE_AXIS_MAX; /* negative = pressed, full brake */
				break;
			case 'q':
				left_hold = BTN_HOLD_TICKS;
				break;
			case 'e':
				right_hold = BTN_HOLD_TICKS;
				break;
			case 'l':
				link_cut = !link_cut;
				printk("> frame stream %s\n", link_cut ? "CUT" : "restored");
				break;
			case '?':
				print_help();
				break;
			}
		}

		if (!link_cut) {
			uint8_t data[8] = {0};

			/* the injector's steering/throttle are 0-based (0 = left, 450 = center;
			 * 0 = idle, up = faster); convert to the wire convention:
			 * steering signed around 0 (negative = left), throttle negative = faster */
			int16_t wire_steer = (steering - STEER_CENTER) * WIRE_AXIS_MAX / STEER_CENTER;
			int16_t wire_thr = -throttle * WIRE_AXIS_MAX / WHEEL_THROTTLE_MAX;

			data[0] = (wire_steer >> 8) & 0xFF;
			data[1] = wire_steer & 0xFF;
			data[2] = (wire_thr >> 8) & 0xFF;
			data[3] = wire_thr & 0xFF;
			data[4] = (brake >> 8) & 0xFF;
			data[5] = brake & 0xFF;
			data[6] = (left_hold ? 0x80 : 0) | (right_hold ? 0x01 : 0);

			uint8_t frame[CMD_FRAME_LEN];

			encode_frame(MSG_WHEEL_STATE, seq++, data, frame);
			uart_send_frame(frame, CMD_FRAME_LEN);
		}

		if (left_hold) {
			left_hold--;
		}
		if (right_hold) {
			right_hold--;
		}

		k_sleep(K_MSEC(SEND_PERIOD_MS));
	}
}

#define INJECT_STACK_SIZE 1024
#define INJECT_PRIORITY   7

K_THREAD_DEFINE(inject_tid, INJECT_STACK_SIZE, inject_thread, NULL, NULL, NULL,
		INJECT_PRIORITY, 0, SYS_FOREVER_MS);

void inject_start(void)
{
	k_thread_start(inject_tid);
}
