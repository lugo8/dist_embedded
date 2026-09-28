#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "uart_common.c"

const char *padding(const char *msg, int len) {
	if (len == pkt_len_no_chksum) {
		return msg;
	}
	unsigned char *result = malloc(pkt_len_no_chksum);
    if (!result) return NULL;

    memcpy(result, msg, len);
    memset(result + len, 0x00, pkt_len_no_chksum - len); 

    return result;
}

int send_packet(const char *msg, int len) {
	//Just sends the input message
	
    //const char *msg = "Hello from Pi\n";
    int n = write(fd, msg, len);
    printf("Wrote %d bytes\n", n);

    return 0;
}

int send_message(const char *msgNoPad, int msgLen) {
	//Adds on padding, then checksum, then sends the message
	
	const char *msgNoChecksum = padding(msgNoPad, msgLen);
	
	unsigned char checksum = checksum_sum(msgNoChecksum, pkt_len_no_chksum); //Get checksum

    //Combine msg and checksum
    char *msgWChecksum = malloc(total_pkt_len);  // new size of msg
    memcpy(msgWChecksum, msgNoChecksum, pkt_len_no_chksum);
    msgWChecksum[total_pkt_len - 1] = checksum; 
    
    //Send message
    print_packet(msgWChecksum, total_pkt_len);
    send_packet(msgWChecksum, total_pkt_len);
}


