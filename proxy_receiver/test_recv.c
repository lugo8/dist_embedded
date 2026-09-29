#include "uart_recv.c"

int main(void) {
    init_uart();
    
    while(1) {
		
		char buf[11];
		get_packet_of_type(buf, 0x04);
		
		printf("End of packet!\n");
	}
	
	close(fd);
    
    return 0;
}
