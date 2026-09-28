/*
 * lab 2 bench test: drives every subsystem from the keyboard over the
 * st-link serial port (115200 baud). once the pi link exists, the command
 * parser calls these same functions instead of the key handler below.
 *
 * modules and what to call:
 *   blinker.h  blinker_init(), blinker_left(), blinker_right(),
 *              blinker_hazard(on), blinker_steer(wheel_deg)
 *   servo.h    servo_init(), servo_set_angle(wheel_deg),
 *              servo_set_us(us)  (always clamped to the linkage limits)
 *   motor.h    motor_init(), motor_forward(), motor_reverse(),
 *              motor_duty(left_pct, right_pct), motor_brake(), motor_coast()
 *   encoder.h  encoder_init(), encoder_rpm(period_ms, &left, &right)
 *              (call at a fixed period; forward is positive on both wheels)
 *   current.h  current_init(), current_read_ma(ma[CUR_COUNT])
 *              (zero is measured at boot, so reset with everything stopped)
 *
 * wheel angle is 0-900 deg: 0 = full left, 450 = center, 900 = full right
 * (lab 1 team-defined values). blinker self-cancel uses the same units.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include "blinker.h"
#include "servo.h"
#include "motor.h"
#include "encoder.h"
#include "current.h"

#define CONTROL_PERIOD_MS 10
/* status line every 200 ms, header every 20 lines so columns stay labeled */
#define PRINT_EVERY 20
#define HEADER_EVERY 20

#define WHEEL_CENTER 450
#define WHEEL_STEP 50

K_TIMER_DEFINE(control_timer, NULL, NULL);

static void print_help(void)
{
	printk("\nmotor:   f fwd  b rev  w/s duty +/-10%%  space brake  x coast\n");
	printk("steer:   a/d wheel -/+50 deg  c center\n");
	printk("blinker: q left  e right  h hazard on/off\n");
	printk("other:   ? help\n\n");
}

static void print_header(void)
{
	printk("%5s %3s | %6s %6s | %6s %6s %6s | %5s %6s\n",
	       "duty", "dir", "rpmL", "rpmR", "mA_L", "mA_R", "mA_S", "wheel", "srv_us");
}

int main(void)
{
	const struct device *con = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t duty = 0;
	char dir = '-';   /* F fwd, R rev, B brake, - coast */
	int wheel = WHEEL_CENTER;
	uint32_t us;
	bool hazard = false;
	unsigned char c;
	int n = 0, lines = 0;

	printk("boot\n");

	/* current zero is measured here, so motors must be coasting and the
	 * servo not yet driven */
	if (encoder_init() || motor_init() || current_init() || servo_init()) {
		printk("init failed\n");
		return 0;
	}
	blinker_init();
	us = servo_set_angle(wheel);

	print_help();
	k_timer_start(&control_timer, K_MSEC(CONTROL_PERIOD_MS), K_MSEC(CONTROL_PERIOD_MS));

	while (1) {
		k_timer_status_sync(&control_timer);

		if (uart_poll_in(con, &c) == 0) {
			switch (c) {
			case 'f':
				dir = 'F';
				motor_forward();
				motor_duty(duty, duty);
				break;
			case 'b':
				dir = 'R';
				motor_reverse();
				motor_duty(duty, duty);
				break;
			case 'w':
			case 's':
				duty = (c == 'w') ? MIN(duty + 10, 100) : (duty >= 10 ? duty - 10 : 0);
				/* only drive if a direction is set, so w/s can't undo a brake */
				if (dir == 'F' || dir == 'R') {
					motor_duty(duty, duty);
				}
				break;
			case ' ':
				dir = 'B';
				motor_brake();
				break;
			case 'x':
				dir = '-';
				motor_coast();
				break;
			case 'a':
			case 'd':
				wheel = CLAMP(wheel + (c == 'd' ? WHEEL_STEP : -WHEEL_STEP), 0, 900);
				us = servo_set_angle(wheel);
				blinker_steer(wheel);
				break;
			case 'c':
				wheel = WHEEL_CENTER;
				us = servo_set_angle(wheel);
				blinker_steer(wheel);
				break;
			case 'q':
				blinker_left();
				printk("> blinker left\n");
				break;
			case 'e':
				blinker_right();
				printk("> blinker right\n");
				break;
			case 'h':
				hazard = !hazard;
				blinker_hazard(hazard);
				printk("> hazard %s\n", hazard ? "on" : "off");
				break;
			case '?':
				print_help();
				lines = 0;
				break;
			}
		}

		int l, r;
		encoder_rpm(CONTROL_PERIOD_MS, &l, &r);

		/* only read currents when printing, to keep the loop light */
		if (++n % PRINT_EVERY == 0) {
			int ma[CUR_COUNT];

			current_read_ma(ma);
			if (lines++ % HEADER_EVERY == 0) {
				print_header();
			}
			printk("%4u%% %3c | %6d %6d | %6d %6d %6d | %5d %6u\n",
			       duty, dir, l, r, ma[CUR_LEFT], ma[CUR_RIGHT], ma[CUR_SERVO], wheel, us);
		}
	}
}