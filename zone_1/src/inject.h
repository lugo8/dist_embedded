#ifndef INJECT_H_
#define INJECT_H_

/* TEMP bench tool: stands in for the Pi. Reads keys from the ST-Link console
 * and sends real MSG_WHEEL_STATE frames out usart1, so the whole path
 * (encode -> UART -> ISR -> ring buf -> parser -> state -> control) is used.
 * Requires PA9 (usart1 TX, D8) jumpered to PA10 (usart1 RX, D2).
 * Delete once the real Pi sender exists.
 */
void inject_start(void);

#endif /* INJECT_H_ */
