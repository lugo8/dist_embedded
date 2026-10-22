#ifndef RX_H_
#define RX_H_

/* Sets up the RX ring buffer and enables the UART RX interrupt on pi_uart.
 * The parser thread itself is auto-started via K_THREAD_DEFINE, so this
 * only needs to run once before frames arrive.
 */
void rx_init(void);

#endif /* RX_H_ */
