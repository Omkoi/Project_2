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

tcp_packet
    *sent_packets[WINDOW_SIZE]; // Store sent packets in the sliding window
sigset_t sigmask;               // Signal mask for the timer

int window_start = 0, window_end = 10, slide_end = 0;
int seqno_to_ack = 0;



int unacknowledged_packets = 0;
int next_seqno = 0;
int duplicate_acks = 0;
int ack_wait = 0;
int previous_received_ackno = 0;

void start_timer();
void stop_timer();
void resend_packets(int sig);
void send_packet(tcp_packet *pkt);
void wait_ack();
void shift_window();
void init_sent_packets();


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



// Send the packet
void send_packet(tcp_packet *pkt) {
  if (sendto(sockfd, pkt, TCP_HDR_SIZE + get_data_size(pkt), 0,
             (const struct sockaddr *)&serveraddr, serverlen) < 0) {
    error("sendto");
  }
  printf("Sent packet with packet number and seqno: %d, %d\n", pkt->hdr.packet_number, pkt->hdr.seqno);
}

void resend_packets(int sig) {
  if (sig == SIGALRM) {
    VLOG(INFO, "Timeout happened. Resending the first packet in the window.");
    tcp_packet *pkt_to_resend = sent_packets[0];
    if (pkt_to_resend->hdr.data_size == 0) {
      printf("End of file reached and acknowledgement received.\n");
      stop_timer();
      close(sockfd);
      exit(0);
    }
    send_packet(pkt_to_resend); // Resend the packet
    start_timer();              // Restart the timer

    //printf("Resending packet %d\n", pkt_to_resend->hdr.seqno);

    //If there is a next packet in the window, set the sequence number to ack to the sequence number of the next packet.
    if (sent_packets[1] != NULL) {
      seqno_to_ack = sent_packets[1]->hdr.seqno;
    }
    //printf("Sequence number to ack: %d\n", seqno_to_ack);
    
  }
  else {
    VLOG(INFO, "3 duplicate ACK happened. Resending the first packet in the window.");
    tcp_packet *pkt_to_resend = sent_packets[0];
    send_packet(pkt_to_resend); // Resend the packet
    start_timer();              // Restart the timer
    printf("Resending packet %d\n", pkt_to_resend->hdr.seqno);
    if (sent_packets[1] != NULL) {
      seqno_to_ack = sent_packets[1]->hdr.seqno;
    }
    printf("Sequence number to ack: %d\n", seqno_to_ack);
  }

}

void shift_window() {
  // Shift the sent packets to the left
  printf("Inside shift window\n");
  for (int i = 0; i < WINDOW_SIZE - 1; i++) {
    if (sent_packets[i+1] !=NULL) {
      sent_packets[i] = sent_packets[i + 1];
      window_end++;
      window_start++;
    }
    else {
      break;
    }
  }
  // Clear the last slot of the window
  sent_packets[WINDOW_SIZE - 1] = NULL;
  printf("Shifted window\n");
}

void wait_ack() {
  char buffer[MSS_SIZE];
  tcp_packet *recvpkt;

  if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr,
               (socklen_t *)&serverlen) < 0) {
    error("recvfrom");
  }
  recvpkt = (tcp_packet *)buffer;
  printf("Received acknowledgement ");
  printf("size and ackno %d, %d\n", recvpkt->hdr.data_size, recvpkt->hdr.ackno);
  if (recvpkt->hdr.ackno == 0) {
    printf("Received final acknowledgement for the end of file\n");
    unacknowledged_packets--;
    printf("Number of unacknowledged packets: %d\n", unacknowledged_packets);
    return;
  }

  //If the receiver acknowledged the previous packet, it asks for the packet that starts with ackno, so we need to remove the acknowledged packet from the window.
  if (recvpkt->hdr.ackno >= sent_packets[0]->hdr.seqno + sent_packets[0]->hdr.data_size) {
    printf("Received ACK: %d,  expected: %d\n", recvpkt->hdr.ackno,
           sent_packets[0]->hdr.seqno + sent_packets[0]->hdr.data_size); //Expected sequence number is the sequence number of the next packet in the window
    //If the acknowledgement is received for the first packet of the window, remove it from the window
    if (recvpkt->hdr.ackno == sent_packets[0]->hdr.seqno + sent_packets[0]->hdr.data_size) {

      if (recvpkt->hdr.ackno == previous_received_ackno) {
        printf("Receive a duplicate acknowledgement\n");
        duplicate_acks = duplicate_acks + 1;
        if (duplicate_acks == 3) {
          resend_packets(duplicate_acks);
        }
      } else {
        duplicate_acks = 0;
      }
      printf("Received acknowledgement for the first packet of the window\n");
      // Stopping the time when the acknowledgement is received for the first packet of the window
      stop_timer();
      sent_packets[0] = NULL;
      unacknowledged_packets--; //Subtract the number of unacknowledged packets
      shift_window();}
    // If the acknowledgement is received for a packet other than the first
    // packet of the window, shift the window upto that point
    else {
      printf("Received acknowledgement for a packet greater than the first "
             "packet of the window\n");
      
    while (sent_packets[0]->hdr.seqno + sent_packets[0]->hdr.data_size != recvpkt->hdr.ackno) {
      shift_window();
      unacknowledged_packets--;
    }

    sent_packets[0] = NULL;
    shift_window();
    unacknowledged_packets--;
    }
    start_timer();
    window_end = window_end + 1;
  }
  //If the received ack is less than the expected seqno, it is a duplicate ack
  else {
    duplicate_acks = duplicate_acks + 1;
    if (duplicate_acks == 3) { //If three times duplicate ack is received, resend the first packet in the window
      resend_packets(duplicate_acks);
    }
  }
  previous_received_ackno = recvpkt->hdr.ackno;
}


void init_sent_packets() {
  for (int i = 0; i < WINDOW_SIZE; i++) {
    sent_packets[i] = NULL;
  }
}


void add_packet_to_window(tcp_packet *pkt) {
  for (int i = 0; i < WINDOW_SIZE; i++) {
    if (sent_packets[i] == NULL) {
      sent_packets[i] = pkt;
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
  init_sent_packets(); // Initialize the sent_packets array
  int eof_reached = 0;

  while (1) {
    
      // If unacknowledged packets are more than the window size, break
    if (unacknowledged_packets >= WINDOW_SIZE) {
      printf("Maximum window size reached. Waiting for acknowledgement\n");
      continue;
    }
    // Else read the next packet from the file to send
    else {
      while ((unacknowledged_packets < WINDOW_SIZE) && (eof_reached == 0)) {
      len = fread(buffer, 1, DATA_SIZE, fp);
      printf("Length: %d\n", len);
      
      if (len <= 0) {
        VLOG(INFO, "End Of File has been reached");
        tcp_packet *sndpkt = make_packet(0);
        add_packet_to_window(sndpkt);
        send_packet(sndpkt);
        start_timer();
        unacknowledged_packets++;
        eof_reached = 1;
        
      }
      else {
        tcp_packet *sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = next_seqno; //Starting byte of the packet
        sndpkt->hdr.packet_number = slide_end; // Tracking the packet number
        send_packet(sndpkt);
        add_packet_to_window(sndpkt);
        start_timer();
        //printf("Packet number added to the window: %d\n", slide_end);
        next_seqno = next_seqno + len;
        unacknowledged_packets++;
        slide_end++;
      }
    }
    //printf("Waiting for acknowledgement\n");
    wait_ack();

    printf("EOF: %d, unacknowledged packets: %d\n", eof_reached,
           unacknowledged_packets);

    if (eof_reached == 1 && unacknowledged_packets == 0) {
      printf("End of file reached and acknowledgement received. Exiting the program\n");
      break;
    }
  }
  }
  fclose(fp);
  close(sockfd);
  return 0;
}
