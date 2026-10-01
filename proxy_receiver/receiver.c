#include <stdio.h> 
#include <stdlib.h> 
#include <unistd.h> 
#include <string.h> 
#include <sys/types.h> 
#include <sys/socket.h> 
#include <arpa/inet.h> 
#include <netinet/in.h> 
#include "pthread.h"
#include <inttypes.h>
#include <time.h>
#include "uart_send.c"
#include "uart_recv.c"
#include "state.h"


// Need to compile with the following flags:
// gcc receiver.c -o proxy_receiver -lgpiod -lpthread

/** Configure this **/
#define LOCAL_HOST "172.26.166.20" // IP of local interface
#define R_PORT 8000

#define REMOTE_HOST "172.26.76.97"
#define S_PORT 8001

// 1 if the wheel/pedals are connected to a Mac (pedals are 0..32767, 0 = no press),
// 0 for the Windows setup (pedals are -32768..32767, 32767 = no press)
#define WHEEL_ON_MAC 0
#define MAX_FORCE_VAL 100
/** **/


#define TX_INTERVAL_MS 300
#define STATE_SIZE sizeof(DIJOYSTATE2_t)

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

uint8_t system_state = 0;

void *send_force(void *arg) {
  printf("start force");
  int8_t force = 0;
  int sockfd;
  struct sockaddr_in servaddr;

  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(S_PORT);
  servaddr.sin_addr.s_addr = inet_addr(REMOTE_HOST);
  
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("failed to create socket");
    exit(EXIT_FAILURE);
  }
	char buf[11];
    struct timespec start, end;
  while (1) {
    
    clock_gettime(CLOCK_MONOTONIC, &start);
    get_packet_of_type(buf, (char)(0x04)); //Get stm32 heartbeat
    clock_gettime(CLOCK_MONOTONIC, &end);
	double elapsed_ms = (end.tv_sec - start.tv_sec) * 1000.0
                       + (end.tv_nsec - start.tv_nsec) / 1e6;
    
    pthread_mutex_lock(&lock);
    if(elapsed_ms >= 60) { //Havent received 3 stm32 packets
		system_state = 1;
		printf("ERROR\n");
	} else {
		system_state = 0;
	}
	pthread_mutex_unlock(&lock);

    
    //Current sensing from the motors
    uint16_t motrL = buf[2] << 8 + buf[3];
    uint16_t motrR = buf[4] << 8 + buf[5];
    uint16_t servo = buf[6] << 8 + buf[7];
    //Zone states
    uint16_t zone_state_A = buf[8];
    uint16_t zone_state_B = buf[9];
    force = ((motrR - motrL) / (65535)) * MAX_FORCE_VAL; //scale the input to be a function of the difference in left and right current
    printf("Right Current: %d | Left Current: %d | Servo Current: %d\n", motrR, motrL, servo);
    printf("Zone A: %d | Zone B: %d\n", zone_state_A, zone_state_B);
    //printf("Send force %d\n", force);
     
    sendto(sockfd, (char*) &force, 1, MSG_CONFIRM,
		    (struct sockaddr *) &servaddr, sizeof(servaddr));

  }
}



int main() {
  printf("Start main\n");
  int sockfd;
  char buffer[STATE_SIZE + 1];
  init_uart(); // initialize uart

  struct sockaddr_in servaddr = { 0 };

  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    perror("failed to create socket");
    exit(EXIT_FAILURE);
  }

  servaddr.sin_family = AF_INET;
  servaddr.sin_port = htons(R_PORT);
  servaddr.sin_addr.s_addr = inet_addr(LOCAL_HOST);

  if (bind(sockfd, (const struct sockaddr *) &servaddr, 
            sizeof(servaddr)) < 0) {
    perror("bind failed");
    exit(EXIT_FAILURE);
  }

  DIJOYSTATE2_t state;
  char recvbuf[sizeof(DIJOYSTATE2_t) + 4];
  pthread_t send_tid;
  pthread_create(&send_tid, NULL, send_force, NULL);

  u_int8_t msgNum = 0;
  while(1) {
    //Receive wheel state and send to rpi
    
    //Wait for wheel state from wheel
    //printf("Wait recv\n");
    int n, len;
    gpio_indicator_set(UDP_RX_INDICATOR_PIN, 1); //Indicate waiting for udp state update
    n = recvfrom(sockfd, recvbuf, STATE_SIZE, MSG_WAITALL,
                  (struct sockaddr *) &servaddr, &len);
    uint32_t packet_ct = ((uint32_t*) recvbuf)[0];
    memcpy(&state, recvbuf + 4, sizeof(state));

#if WHEEL_ON_MAC
    // Mac reports pedals as 0 (no press) .. 32767 (fully down); zone_1 expects
    // 32767 (no press) .. -32768 (fully down). Wheel range is already close enough.
    state.lY  = 32767 - 2 * (int32_t) state.lY;
    state.lRz = 32767 - 2 * (int32_t) state.lRz;
    state.lX = 32767 - 2 * (int32_t) state.lX;
    if (state.lX  < -32768) state.lX  = -32768;
    if (state.lY  < -32768) state.lY  = -32768;
    if (state.lRz < -32768) state.lRz = -32768;
#endif

    //Print wheel state
    if (0) { 
		printf("Receive state (Pkt: %8X) :  Wheel: %d | Throttle: %d | Brake: %d | \nA Btn: %d | B Btn: %d| X Btn: %d| Y Btn: %d| \nR Paddle: %d | L Paddle: %d | RSB: %d | LSB: %d| \n3 Lines: %d | 2 Boxes: %d | XBOX: %d | \n", 
		packet_ct, state.lX, state.lY, state.lRz, 
		state.rgbButtons[0], state.rgbButtons[1], state.rgbButtons[2], state.rgbButtons[3], state.rgbButtons[4],
		state.rgbButtons[5], state.rgbButtons[8], state.rgbButtons[9], state.rgbButtons[6], state.rgbButtons[7],
		state.rgbButtons[10]);
	}
    //Make button byte
    u_int8_t btnByte = 0;
    if (state.rgbButtons[4]) { //rightmost bit means right turn signal
      btnByte += 1;
    } 
    
    if (state.rgbButtons[5]) { //leftmost bit means left turn signal
      btnByte += 128;
    } 
    
    if (state.rgbButtons[0]) { // 5th bit means a btn pressed and front should error
      btnByte += 16;
    }
    
    if (state.rgbButtons[1]) { // 4th bit B btn = zone_1 self-test (BTN_BIT_SELF_TEST in protocol.h)
      btnByte += 8;
    }
    
    if (state.rgbButtons[2]) { // 3th bit means X btn a pressed and RPi should error
      btnByte += 4;
    }
    
    if (state.rgbButtons[3]) { //6th bit means Y btn: inject an out-of-range command (bit 5 is reserved)
      btnByte += 32;
    }

    //Send packet
    // type | number | steering | throttle | break | button byte | checksum
    pthread_mutex_lock(&lock);
    unsigned char msg[10] = {
      0x01,
      (unsigned char)(msgNum++),
      (unsigned char)((state.lX >> 8) & 0xFF), 
      (unsigned char)(state.lX & 0xFF),
      (unsigned char)((state.lY >> 8) & 0xFF),
      (unsigned char)(state.lY & 0xFF),
      (unsigned char)((state.lRz >> 8) & 0xFF),
      (unsigned char)(state.lRz & 0xFF),
      btnByte, 
      system_state
    };
    pthread_mutex_unlock(&lock);
    
    uint8_t timingDebug = 1;
    uint8_t debugBtnVal = state.rgbButtons[1]; //0 is a, 1 is b
    if(timingDebug && debugBtnVal == 128) { //Only send packets when B is pressed
		send_message(msg, 10); //set new update
	}
	
	else {
		send_message(msg, 10);
	}
  }

  return 0;
}
