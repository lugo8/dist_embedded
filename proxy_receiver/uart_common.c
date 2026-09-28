#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <unistd.h>


int fd = -1;
const u_int8_t data_len = 8; //8 bytes in the data section of each packet
const u_int8_t pkt_len_no_chksum = data_len + 2;
const u_int8_t total_pkt_len = pkt_len_no_chksum + 1;

void print_packet(const unsigned char *buf, size_t len) {
	for (size_t i = 0; i < len; i++) {
		printf("%02X  ", buf[i]);
	}
	printf("\n");
}
unsigned char checksum_sum(const unsigned char *data, size_t len) {
    unsigned char sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];   // wraps naturally at 256 since it's unsigned char
    }
    return sum;
}

int uart_open(const char *device, speed_t baud) {
    fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    // Clear NDELAY so reads block per VMIN/VTIME below
    fcntl(fd, F_SETFL, 0);

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, baud);
    cfsetispeed(&tty, baud);

    tty.c_cflag &= ~PARENB;        // no parity
    tty.c_cflag &= ~CSTOPB;        // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;            // 8 data bits
    tty.c_cflag &= ~CRTSCTS;       // no hardware flow control
    tty.c_cflag |= CREAD | CLOCAL; // enable receiver, ignore modem lines

    tty.c_lflag &= ~ICANON;        // raw mode, not line-buffered
    tty.c_lflag &= ~ECHO;
    tty.c_lflag &= ~ECHOE;
    tty.c_lflag &= ~ISIG;

    tty.c_iflag &= ~(IXON | IXOFF | IXANY);  // no software flow control
    tty.c_iflag &= ~(ICRNL | INLCR);         // don't translate CR/NL

    tty.c_oflag &= ~OPOST;         // raw output

    tty.c_cc[VMIN]  = 0;   // non-blocking-ish read
    tty.c_cc[VTIME] = 10;  // 1 second read timeout (in deciseconds)

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        close(fd);
        return -1;
    }

    return fd;
}

int init_uart(void) {
	uart_open("/dev/serial0", B115200);
	
    if (fd == -1) return 1;
    return fd;
}
