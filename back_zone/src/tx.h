#ifndef TX_H_
#define TX_H_

/* Starts the periodic steering-zone heartbeat transmitter on both CAN buses.
 * Call after rx_init(), which starts both CAN controllers.
 */
int tx_start(void);

#endif /* TX_H_ */
