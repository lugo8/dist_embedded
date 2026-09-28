#include "uart_recv.c"

int main(void) {
    init_uart();
    
    while(1) {
		
		char buf[256];
		get_packet(buf);
		
		printf("End of packet!\n");
	}
	
	close(fd);
    
    return 0;
}
