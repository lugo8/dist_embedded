#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "uart_common.c"

int read_exact(int fd, unsigned char *buf, size_t n) {
    //Used for calculating the timeout
    size_t total = 0;
    float timeout_ms = 0.1; //Timeout in ms 
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    
    while (total < n) {
		//Get values and determine if we made it to the desired number of values
        int r = read(fd, buf + total, n - total);
        if (r > 0) {
            total += r; 
            continue;     
        }
        
        //Calculate time difference for timeout
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed_ms = (now.tv_sec - start.tv_sec) * 1000.0
                           + (now.tv_nsec - start.tv_nsec) / 1e6;

        if (elapsed_ms > timeout_ms) {
            //printf("Error: Did not reach end of packet (timeout)\n");
            return -1;
        }


    }
    
    if (total == n) {
		
		return total; //got all symbols
	}
	
	return -1; //missed some
}

int resync(unsigned char *buf) {
    while (1) {
        int n = read_exact(fd, buf, 1);  // this succeeds immediately if data is flowing
        if (n <= 0) return -1;            // only fails if line is genuinely idle
        //if (buf[0] == 0x01) return 1;     // found sync byte
        // wrong byte — discard and immediately try the next one, no delay
    }
}

char* get_packet(char *buf) {
	//Function to get a packet from uart
	//Buf must be big enough to account for the largest data len

    //printf("Listening on /dev/serial0...\n");

	unsigned char window[total_pkt_len];
	read_exact(fd, window, total_pkt_len);  // get 10 vbytes

	while (1) {
		//Test checksum
		unsigned char checksum = checksum_sum(window, total_pkt_len - 1);
		if (checksum == window[total_pkt_len - 1]) {
			memcpy(buf, window, total_pkt_len);
			
			//print out the received data
			printf("Received: ");
			print_packet(buf, total_pkt_len);
				
			return (char*)buf;
		}

		// slide window forward by 1 byte if it doesnt pass checksum
		memmove(window, window + 1, total_pkt_len - 1);
		read_exact(fd, window + total_pkt_len - 1, 1);  // read one new byte
	}
}

