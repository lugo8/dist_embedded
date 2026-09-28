#include "uart_send.c"

int main(void) {
	init_uart();
	const char msg[] = {0x01, 0x00, 0x07, 0xff, 0xab, 0x23, 0x45, 0x12, 0xcd, 0xff};
	
    send_message(msg, 10);
    usleep(10 * 1000);
    const char msg2[] = {0x01, 0x00, 0x08};
	
    send_message(msg2, 3);
   
    usleep(10 * 1000);
    
    send_packet(msg, 10);
    
    usleep(10 * 1000);
    const char msg3[] = {0x01, 0x00, 0x07, 0xff, 0xab, 0x23, 0x45, 0x12, 0xcd, 0xff};
	
    send_message(msg3, 10);
    
    usleep(10 * 1000);
    
    const char msg4[] = {0x01, 0x00, 0x07, 0xff, 0xab, 0x23, 0x45, 0x12, 0xcd, 0xff};
	
	for(size_t i = 0; i < 3; i++) {
		send_message(msg4, 10);
	}
    
    close(fd);
    
    return 0;
}
