#include "uart_send.c"

int main(void) {
	init_uart();
	const char msg[] = {0x05, 0x00, 0x7f};
	
	while(1) {
		send_message(msg, 3);
	}
    
   
    
    
    close(fd);
    
    return 0;
}
