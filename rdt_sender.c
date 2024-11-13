#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 10 // Fixed window size
#define RETRY 120      // Timeout in milliseconds

int sockfd, serverlen;
struct sockaddr_in serveraddr;
int window_start = 0, window_end = 0;
tcp_packet
    *sent_packets[WINDOW_SIZE]; // Store sent packets in the sliding window
sigset_t sigmask;               // Signal mask for the timer

void start_timer();
void stop_timer();
void resend_packets(int sig);
void send_packet(tcp_packet *pkt);
void wait_for_ack(int expected_seqno);

// Initialize and start the timer
void init_timer(int delay, void (*sig_handler)(int)) {
  signal(SIGALRM, sig_handler);
  struct itimerval timer;
  timer.it_interval.tv_sec = delay / 1000;
  timer.it_interval.tv_usec = (delay % 1000) * 1000;
  timer.it_value.tv_sec = delay / 1000;
  timer.it_value.tv_usec = (delay % 1000) * 1000;
  sigemptyset(&sigmask);
  sigaddset(&sigmask, SIGALRM);
  setitimer(ITIMER_REAL, &timer, NULL);
}

// Start the timer to handle retransmissions
void start_timer() { sigprocmask(SIG_UNBLOCK, &sigmask, NULL); }

// Stop the timer
void stop_timer() { sigprocmask(SIG_BLOCK, &sigmask, NULL); }

// Resend the oldest unacknowledged packet upon timeout
void resend_packets(int sig) {
  if (sig == SIGALRM) {
    VLOG(INFO, "Timeout happened");
    tcp_packet *pkt_to_resend = sent_packets[window_start % WINDOW_SIZE];
    send_packet(pkt_to_resend); // Resend the packet
    start_timer();              // Restart the timer
  }
}

// Send the packet and add it to the window
void send_packet(tcp_packet *pkt) {

  if (sendto(sockfd, pkt, TCP_HDR_SIZE + get_data_size(pkt), 0,
             (const struct sockaddr *)&serveraddr, serverlen) < 0) {
    error("sendto");
  }
  sent_packets[window_end % WINDOW_SIZE] = pkt;
  window_end++;
}

// Wait for acknowledgment and handle sliding the window
void wait_for_ack(int expected_seqno) {
  char buffer[MSS_SIZE];
  tcp_packet *recvpkt;

  while (1) {
    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr,
                 (socklen_t *)&serverlen) < 0) {
      error("recvfrom");
    }

    recvpkt = (tcp_packet *)buffer;
    VLOG(DEBUG, "Received ACK: %d", recvpkt->hdr.ackno);

    if (recvpkt->hdr.ackno > window_start) {
          printf("Sliding window: Acknowledged %d, Window Start: %d, Window End: %d\n", recvpkt->hdr.ackno, window_start, window_end);

      window_start = window_start + 1;
      // Stop the timer when the last unacknowledged packet is acknowledged
      if (window_start == window_end - 1) {
        stop_timer();
      }
      break;
    }
  }
}

int main(int argc, char **argv) {
  int portno, len;
  char *hostname;
  char buffer[DATA_SIZE];
  FILE *fp;

  if (argc != 4) {
    fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
    exit(0);
  }
  hostname = argv[1];
  portno = atoi(argv[2]);
  fp = fopen(argv[3], "r");
  if (fp == NULL) {
    error(argv[3]);
  }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  bzero((char *)&serveraddr, sizeof(serveraddr));
  serverlen = sizeof(serveraddr);

  if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
    fprintf(stderr, "ERROR, invalid host %s\n", hostname);
    exit(0);
  }

  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(portno);

  init_timer(RETRY, resend_packets);
  int next_seqno = 0;

  while (1) {
    len = fread(buffer, 1, DATA_SIZE, fp);
    if (len <= 0) {
      VLOG(INFO, "End Of File has been reached");
      tcp_packet *sndpkt = make_packet(0);
      send_packet(sndpkt);
      break;
    }

    tcp_packet *sndpkt = make_packet(len);
    memcpy(sndpkt->data, buffer, len);
    sndpkt->hdr.seqno = next_seqno;

    // Send the packet if the window allows
    if (window_end - window_start < WINDOW_SIZE) {
      send_packet(sndpkt);
      printf("Window Start: %d, Window End: %d\n", window_start, window_end);
      printf("Sending packet %d\n", sndpkt->hdr.seqno);
    }

    // Wait for ACK of the first packet in the window
    wait_for_ack(next_seqno);

    next_seqno += len;
  }

  fclose(fp);
  return 0;
}
