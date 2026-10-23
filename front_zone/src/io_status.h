#ifndef IO_STATUS_H_
#define IO_STATUS_H_

#include <stdbool.h>
#include <stdint.h>

int io_status_init(void);
int io_status_can_rx(uint8_t bus);
int io_status_can_tx(uint8_t bus);
int io_status_set_can_degraded(uint8_t bus, bool degraded);
void io_status_enter_error(uint8_t zone_state);
void io_status_clear_error(void);
bool io_status_error_active(void);
void io_status_note_control_received(void);
void io_status_mark_error(void);
bool io_status_blinker_hazard_override(void);

#endif /* IO_STATUS_H_ */
