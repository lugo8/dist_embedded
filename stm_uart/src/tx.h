#ifndef TX_H_
#define TX_H_

#include <stddef.h>
#include <stdint.h>

/* Sends a whole frame byte-by-byte over pi_uart, serialized against other
 * senders. Shared with the TEMP loopback test thread in loss_test.c, which
 * also transmits on pi_uart - interleaved bytes from two threads mid-frame
 * would corrupt both.
 */
void uart_send_frame(const uint8_t *frame, size_t len);

#endif /* TX_H_ */
