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

int duplicate_acks = 0;

int sockfd, serverlen;
struct sockaddr_in serveraddr;
int window_start = 0, window_end = 10, slide_end = 0;
int seqno_to_ack = 0;
int ack_wait = 0;
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
    seqno_to_ack = sent_packets[window_start % WINDOW_SIZE]->hdr.seqno + sent_packets[window_start % WINDOW_SIZE]->hdr.data_size;
    printf("Sequence number to ack: %d\n", seqno_to_ack);
    tcp_packet *pkt_to_resend = sent_packets[window_start % WINDOW_SIZE];
    send_packet(pkt_to_resend); // Resend the packet
    printf("Resending packet %d\n", pkt_to_resend->hdr.seqno);
    start_timer();              // Restart the timer
  }
  else {
    VLOG(INFO, "3 duplicate ACK happened");
    printf("Sequence number to ack: %d\n", seqno_to_ack);
    tcp_packet *pkt_to_resend = sent_packets[window_start % WINDOW_SIZE];
    send_packet(pkt_to_resend); // Resend the packet
    printf("Resending packet %d\n", pkt_to_resend->hdr.seqno);
    start_timer();              // Restart the timer
  }

}

// Send the packet and add it to the window
void send_packet(tcp_packet *pkt) {

  if (sendto(sockfd, pkt, TCP_HDR_SIZE + get_data_size(pkt), 0,
             (const struct sockaddr *)&serveraddr, serverlen) < 0) {
    error("sendto");
  }
  sent_packets[slide_end % WINDOW_SIZE] = pkt;
}

// Wait for acknowledgment and handle sliding the window
void wait_for_ack(int expected_seqno) {
  char buffer[MSS_SIZE];
  tcp_packet *recvpkt;

  while (1) {


    int read_bytes =
        (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr,
                  (socklen_t *)&serverlen));
   // printf("Read bytes: %d\n", read_bytes);
    if (read_bytes < 0) {
      error("recvfrom");
    }
    else if (read_bytes == 0) {
      printf("All Acknowledgements received\n");
      exit(0);
    }

    recvpkt = (tcp_packet *)buffer;
    VLOG(DEBUG, "Received ACK: %d", recvpkt->hdr.ackno);

    if (recvpkt->hdr.ackno >= seqno_to_ack) {
      printf("I am here\n");
      ack_wait = 0;
      window_start = window_start + 1;
      window_end = window_end + 1;

      if (window_start == slide_end) {
        stop_timer();
        printf("Here. getting out of loop\n");
        break;
      }
      seqno_to_ack = recvpkt->hdr.ackno + recvpkt->hdr.data_size; 
      //seqno_to_ack = sent_packets[window_start % WINDOW_SIZE]->hdr.seqno + sent_packets[window_start % WINDOW_SIZE]->hdr.data_size;

      printf("Sequence number to ack: %d\n", seqno_to_ack);
      printf(
          "Sliding window: Acknowledged %d, Window Start: %d, Slide End: %d, Window End: %d\n",
          recvpkt->hdr.ackno, window_start, slide_end, window_end);

      // Stop the timer when the last unacknowledged packet is acknowledged
      if (window_start == slide_end) {
        stop_timer();
      }
      break;
    } else {
      printf("I am here 2\n");
      printf("Received ACK: %d, but expected: %d\n", recvpkt->hdr.ackno,
             seqno_to_ack);
      ack_wait = ack_wait + 1;
      printf("Ack wait: %d\n", ack_wait);
      if (ack_wait == 3) {
        duplicate_acks = 1;
        resend_packets(duplicate_acks);
        ;
        break;
      }
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
  int eof_reached = 0;

  while (1) {
    while (window_end - slide_end > 0) {
      len = fread(buffer, 1, DATA_SIZE, fp);
      printf("Length: %d\n", len);
      printf("EOF reached: %d\n", eof_reached);

      if (eof_reached == 1) {
        break;
      }
      
      if (len <= 0 && eof_reached == 0) {
        VLOG(INFO, "End Of File has been reached");
        tcp_packet *sndpkt = make_packet(0);
        send_packet(sndpkt);
        eof_reached = 1;
        break;
      } else {
        tcp_packet *sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = next_seqno;
        sndpkt->hdr.packet_number = slide_end;
        next_seqno = next_seqno + len;

        if (slide_end == 0) {
          seqno_to_ack = len;
        }

        send_packet(sndpkt);
        slide_end++;
        //printf("Increasing slide end\n");
        printf("Window Start: %d, Slide End: %d\n", window_start, slide_end);
        printf("Sending packet with packet number and seqno: %d, %d\n",
               sndpkt->hdr.packet_number, sndpkt->hdr.seqno);
      }
    }

    printf("Waiting for ACK %d\n", seqno_to_ack);
    wait_for_ack(seqno_to_ack);

    if (len <= 0 && window_start == slide_end) {
      printf("I am here. Window start and slide end are equal\n");
      break;
    }
  }

  fclose(fp);
  return 0;

}