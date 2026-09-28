#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/ring_buffer.h>

static const struct device *const pi_uart = DEVICE_DT_GET(DT_NODELABEL(usart1));


#define RX_RING_BUF_SIZE 64 // ring stores 64 bytes
static uint8_t rx_ring_buf_data[RX_RING_BUF_SIZE];
static struct ring_buf rx_ring_buf;

static K_SEM_DEFINE(rx_sem, 0, 1); // binary for now

// frame: [msg type(1B)][seq(1B)][data(8B)][checksum(1B)]
#define MSG_WHEEL_STATE 0x01 // steering(2B) throttle(2B) brake(2B) btn(1B)
#define MSG_HB_RPI      0x02 // line_alive(1B): bit7=alive
#define MSG_HB_DRIVETRAIN 0x03 // line_alive(1B): bit7=alive
#define MSG_STATUS      0x04 // motor_a_current(2B) motor_b_current(2B) servo_current(2B) zone_state(1B) reserved(1B)
#define MSG_FORCE_FDBK  0x05 // force(2B)

#define CMD_CHK_LEN   (10) // msg_type + seq + data
#define CMD_FRAME_LEN (CMD_CHK_LEN + 1)      // + checksum

// timing stuff
#define LINK_TIMEOUT_MS 150
#define STATUS_PERIOD_MS 20

struct wheel_state {
	uint8_t seq;
	uint16_t steering;
	uint16_t throttle;
	uint16_t brake;
	bool left_btn;
	bool right_btn;
};

struct heartbeat {
	uint8_t seq;
	bool line_alive;
};

struct force_feedback {
	uint8_t seq;
	int16_t force;
};

enum zone_state {
	ZONE_STATE_NORMAL = 0,
	ZONE_STATE_FAILSAFE_LINK_LOST = 1,
	ZONE_STATE_FAILSAFE_SELF_TEST = 2, // add when there's button
	ZONE_STATE_FAILSAFE_BAD_CMD = 3,   // for out-of-range detection
};

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


static uint8_t checksum(const uint8_t *data, size_t len)
{
	uint8_t sum = 0;

	for (size_t i = 0; i < len; i++) {
		sum ^= data[i];
	}

	return sum;
}

/* --------------------------------- RX --------------------------------- */
// check if valid msg
static bool is_known_msg_type(uint8_t msg_type)
{
	switch (msg_type) {
		case MSG_WHEEL_STATE:
			return true;
		case MSG_HB_RPI:
			return true;
		case MSG_HB_DRIVETRAIN:
			return true;
		case MSG_FORCE_FDBK:
			return true;
		case MSG_STATUS: // TEMP: only relevant on the self-loopback test rig -
			return true; // the real Pi never echoes this back to the STM32
		default:
			return false;
	}
}

// get wheel info
static void decode_wheel_state(const uint8_t *frame, struct wheel_state *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->steering = (data[0] << 8 | data[1]); 
	out->throttle = (data[2] << 8 | data[3]);
	out->brake = (data[4] << 8 | data[5]);
	out->left_btn = data[6] >> 7;
	out->right_btn = data[6] & 0x01;
}

// get heartbeat info
static void decode_heartbeat(const uint8_t *frame, struct heartbeat *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->line_alive = data[0] >> 7;
}

// get force feedback info
static void decode_force_feedback(const uint8_t *frame, struct force_feedback *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->force = (data[0] << 8 | data[1]);
}

// TEMP: loopback test only - real Pi never sends this back to the STM32
struct status_frame {
	uint8_t seq;
	uint16_t motor_a_current;
	uint16_t motor_b_current;
	uint16_t servo_current;
	uint8_t zone_state;
};

static void decode_status(const uint8_t *frame, struct status_frame *out)
{
	const uint8_t *data = &frame[2];

	out->seq = frame[1];
	out->motor_a_current = (data[0] << 8 | data[1]);
	out->motor_b_current = (data[2] << 8 | data[3]);
	out->servo_current = (data[4] << 8 | data[5]);
	out->zone_state = data[6];
}

static const char *zone_state_name(uint8_t zone_state)
{
	switch (zone_state) {
	case ZONE_STATE_NORMAL:
		return "NORMAL";
	case ZONE_STATE_FAILSAFE_LINK_LOST:
		return "FAILSAFE_LINK_LOST";
	case ZONE_STATE_FAILSAFE_SELF_TEST:
		return "FAILSAFE_SELF_TEST";
	case ZONE_STATE_FAILSAFE_BAD_CMD:
		return "FAILSAFE_BAD_CMD";
	default:
		return "UNKNOWN";
	}
}

// check if link is alive
static bool link_is_alive(void)
{
	int64_t last;

	k_mutex_lock(&wheel_state_mutex, K_FOREVER);
	last = last_good_frame_ms;
	k_mutex_unlock(&wheel_state_mutex);

	return (k_uptime_get() - last) < LINK_TIMEOUT_MS;
}

// read CMD_FRAME_LEN bytes and decode them
static void link_rx_thread(void *p1, void *p2, void *p3)
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
				printk("link_rx_thread: bad checksum (type 0x%02x)\n", msg_type);
				continue;
			}

			switch (msg_type) {
				case MSG_WHEEL_STATE: {
					struct wheel_state ws;

					decode_wheel_state(frame, &ws);

					k_mutex_lock(&wheel_state_mutex, K_FOREVER);
					latest_wheel_state = ws;
					last_good_frame_ms = k_uptime_get();
					k_mutex_unlock(&wheel_state_mutex);
					break;
				}
				case MSG_HB_RPI: {
					struct heartbeat hb;

					decode_heartbeat(frame, &hb);

					k_mutex_lock(&aux_mutex, K_FOREVER);
					latest_hb_rpi = hb;
					last_rpi_hb_ms = k_uptime_get();
					k_mutex_unlock(&aux_mutex);
					break;
				}
				case MSG_HB_DRIVETRAIN: {
					struct heartbeat hb;

					decode_heartbeat(frame, &hb);

					k_mutex_lock(&aux_mutex, K_FOREVER);
					latest_hb_drivetrain = hb;
					last_drivetrain_hb_ms = k_uptime_get();
					k_mutex_unlock(&aux_mutex);
					break;
				}
				case MSG_FORCE_FDBK: {
					struct force_feedback fb;

					decode_force_feedback(frame, &fb);

					k_mutex_lock(&aux_mutex, K_FOREVER);
					latest_force_feedback = fb;
					k_mutex_unlock(&aux_mutex);
					break;
				}
				case MSG_STATUS: {
					struct status_frame sf;

					decode_status(frame, &sf);

					/* TEMP: loopback test - dump decoded STATUS fields */
					printk("status: seq=%u motor_a=%u motor_b=%u servo=%u zone_state=%s\n",
					       sf.seq, sf.motor_a_current, sf.motor_b_current,
					       sf.servo_current, zone_state_name(sf.zone_state));
					break;
				}
				default: {
					printk("WRONG MSG\n");
				}
			}
		}
	}
}

/* --------------------------------- TX --------------------------------- */
#define LINK_RX_STACK_SIZE 1024
#define LINK_RX_PRIORITY 5 // TODO: placeholder

K_THREAD_DEFINE(link_rx_tid, LINK_RX_STACK_SIZE, link_rx_thread, NULL, NULL, NULL,
		        LINK_RX_PRIORITY, 0, 0);

// form payload
static void encode_status_data(uint16_t motor_a_current, uint16_t motor_b_current,
				               uint16_t servo_current, uint8_t zone_state, uint8_t data[8])
{
	data[0] = motor_a_current >> 8;
	data[1] = motor_a_current & 0xFF;
	data[2] = motor_b_current >> 8;
	data[3] = motor_b_current & 0xFF;
	data[4] = servo_current >> 8;
	data[5] = servo_current & 0xFF;
	data[6] = zone_state;
	data[7] = 0;
}

// form frame
static void encode_frame(uint8_t msg_type, uint8_t seq, const uint8_t data[8], uint8_t *frame_out)
{
	frame_out[0] = msg_type;
	frame_out[1] = seq;

	for (int i = 0; i < 8; i++) {
		frame_out[2 + i] = data[i];
	}

	frame_out[CMD_FRAME_LEN - 1] = checksum(frame_out, CMD_CHK_LEN);
}

// serializes whole-frame sends - status_tx_thread and the link-loss test
// thread both transmit on pi_uart, and interleaved bytes from two threads
// mid-frame would corrupt both
static K_MUTEX_DEFINE(uart_tx_mutex);

// send byte by byte
static void uart_send_frame(const uint8_t *frame, size_t len)
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

	while (1) {
		// TODO: replace with real ADC current-sensor reads
		uint16_t motor_a_current = 0;
		uint16_t motor_b_current = 0;
		uint16_t servo_current = 0;
		uint8_t zone_state = link_is_alive() ? ZONE_STATE_NORMAL : ZONE_STATE_FAILSAFE_LINK_LOST; // TODO: update later

		uint8_t data[8];
		encode_status_data(motor_a_current, motor_b_current, servo_current,
				    zone_state, data);

		uint8_t frame[CMD_FRAME_LEN];
		encode_frame(MSG_STATUS, seq++, data, frame);
		uart_send_frame(frame, CMD_FRAME_LEN);

		k_sleep(K_MSEC(STATUS_PERIOD_MS));
	}
}

#define STATUS_TX_STACK_SIZE 1024
#define STATUS_TX_PRIORITY   6 /* placeholder - revisit against the Part 4 task table */

K_THREAD_DEFINE(status_tx_tid, STATUS_TX_STACK_SIZE, status_tx_thread, NULL, NULL, NULL,
		 STATUS_TX_PRIORITY, 0, 0);

/* TEMP: loopback test - alternates between bursting MSG_WHEEL_STATE frames
 * (keeps link_is_alive() true) and going silent for longer than
 * LINK_TIMEOUT_MS (forces it false). Watch the "status: ... zone_state=..."
 * prints flip between NORMAL and FAILSAFE_LINK_LOST accordingly.
 * Requires PB6 (usart1 TX) jumpered to PB7 (usart1 RX) on the Nucleo.
 * Delete this thread once the real Pi-side sender exists.
 */
#define LINK_LOSS_TEST_SEND_MS 2000
#define LINK_LOSS_TEST_GAP_MS  2000

static void link_loss_test_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t seq = 0;

	while (1) {
		printk("link_loss_test: sending wheel_state for %dms (expect zone_state=NORMAL)\n",
		       LINK_LOSS_TEST_SEND_MS);

		int64_t send_until = k_uptime_get() + LINK_LOSS_TEST_SEND_MS;

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

		printk("link_loss_test: going silent for %dms (expect zone_state -> FAILSAFE_LINK_LOST)\n",
		       LINK_LOSS_TEST_GAP_MS);

		k_sleep(K_MSEC(LINK_LOSS_TEST_GAP_MS));
	}
}

#define LINK_LOSS_TEST_STACK_SIZE 1024
#define LINK_LOSS_TEST_PRIORITY   7

K_THREAD_DEFINE(link_loss_test_tid, LINK_LOSS_TEST_STACK_SIZE, link_loss_test_thread, NULL, NULL, NULL,
		 LINK_LOSS_TEST_PRIORITY, 0, 0);

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

int main(void)
{
	printk("START MAIN\n");
	if (!device_is_ready(pi_uart)) {
		printk("usart1 (Pi link) not ready\n");
		return 0;
	}

	printk("DEVICE DETECTED\n");

	ring_buf_init(&rx_ring_buf, sizeof(rx_ring_buf_data), rx_ring_buf_data);
	printk("RING INITED\n");
	uart_irq_callback_user_data_set(pi_uart, pi_uart_isr, NULL);
	uart_irq_rx_enable(pi_uart);

	printk("car_fw up, usart1 ready\n");

	while (1) {
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
