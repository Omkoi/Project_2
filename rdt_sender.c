#include <arpa/inet.h>

#include <signal.h>

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <sys/socket.h>

#include <sys/time.h>

#include <unistd.h>

#include "common.h"

#include "packet.h"

#define WINDOW_SIZE 10 // Fixed window size

#define RETRY 120 // Timeout in milliseconds

int sockfd, serverlen;

struct sockaddr_in serveraddr;

tcp_packet

    *sent_packets[WINDOW_SIZE]; // Store sent packets in the sliding window

sigset_t sigmask; // Signal mask for the timer

int slide_end = 0;              // The number of the last packet in the window
int seqno_to_ack = 0;           // The sequence number to be acknowledged
int unacknowledged_packets = 0; // The number of unacknowledged packets
int next_seqno = 0;     // The sequence number of the next packet to be sent
int duplicate_acks = 0; // The number of duplicate ACKs

void start_timer();
void stop_timer();
void resend_packets(int sig);
void send_packet(tcp_packet *pkt);
void wait_ack();
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

  // Sending the packet to the receiver
  VLOG(DEBUG, "Sending packet %d to %s", pkt->hdr.seqno,
       inet_ntoa(serveraddr.sin_addr));
  if (sendto(sockfd, pkt, TCP_HDR_SIZE + get_data_size(pkt), 0,

             (const struct sockaddr *)&serveraddr, serverlen) < 0) {

    error("sendto");
  }
}

// Handle the resending of packets due to timeout and duplicate ACKs
void resend_packets(int sig) {
  if (sig == SIGALRM) {
    VLOG(INFO, "Timeout happened.");
    // The packet to be resent is the first unacknowledged packet in the
    // sent_packets window.
    tcp_packet *pkt_to_resend = sent_packets[0];
    // Stopping the timer since the timeout has occurred
    stop_timer();
    if (pkt_to_resend->hdr.data_size == 0) {
      // If the acknoledgement is received for the end of file, print a message
      // and exit
      //printf("End of file reached and acknowledgement received inside "
             //"resend_packets.\n");

      // Stopping the timer and closing the socket

      stop_timer();
      //sleep(2);
      //close(sockfd); // Closing the socket
      //exit(0);       // Exiting the program
    }

    send_packet(pkt_to_resend); // Resend the packet
    start_timer();              // Restarting the timer

    if (sent_packets[1] != NULL) {
      // Updating the sequence number to be acknowledged
      seqno_to_ack = sent_packets[1]->hdr.seqno;
    }
    // If null, no next packets in the sent array.
  } else {
    // If 3 duplicate ACKs are received, resend the first packet in the window.
    // The packet to be resent is the first packet in the sent_packets window.
    tcp_packet *pkt_to_resend = sent_packets[0];
    // Stopping the timer since the timeout has occurred
    stop_timer();
    // Resending the packet
    send_packet(pkt_to_resend);
    // Restarting the timer
    start_timer(); // Restart the timer
    // Updating the sequence number to be acknowledged
    if (sent_packets[1] != NULL) {
      seqno_to_ack = sent_packets[1]->hdr.seqno;
    }
  }
}

// Wait for the acknowledgement
void wait_ack() {
  char buffer[MSS_SIZE];
  tcp_packet *recvpkt;

  // Receiving the acknowledgement
  if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&serveraddr,

               (socklen_t *)&serverlen) < 0) {
    error("recvfrom");
  }

  // Logging the acknowledgement
  VLOG(INFO, "0");
  // Converting the buffer to a tcp packet
  recvpkt = (tcp_packet *)buffer;
  // If the acknowledgement is for the end of file, print a message and the
  // number of unacknowledged packets Timer reseting after the acknowledgement
  // is received


  if (sent_packets[0] != NULL && recvpkt->hdr.ackno == -1 &&
      sent_packets[0]->hdr.data_size == 0) {
    printf(
        "End of File Reached and Acknowledged.\n");
    close(sockfd);
    exit(0);
    stop_timer();
  }


  int i = 0;
  // Finding the first packet in the window that is not acknowledged
  for (i = 0; i < WINDOW_SIZE; i++) {
    if (sent_packets[i] == NULL ||
        sent_packets[i]->hdr.seqno >= recvpkt->hdr.ackno) {
      // if the index in the sent packet array is null or the acknowledgement is
      // for the already acknowledged packet, break
      break;
    }
  }
  // Else acknowledge the packet and shift everything left by i packets
  for (int j = 0; j < WINDOW_SIZE - i; j++) {
    if (j + i >= WINDOW_SIZE) {
      // If the index is out of bounds, set the slot to null
      sent_packets[j] = NULL;
    } else {
      // Else shift the packet to the left by i packets
      sent_packets[j] = sent_packets[j + i];
    }
  }
  // Setting the rest of the slots to null
  for (int j = WINDOW_SIZE - i; j < WINDOW_SIZE; j++) {
    sent_packets[j] = NULL;
  }

  // If no packets are acknowledged from the window, increment the duplicate
  // acknowledgement counter as the previous acknowledgement was received again
  if (i == 0) {
    duplicate_acks += 1;
    // If 3 duplicate acknowledgements are received, resend the first packet in
    // the window
    if (duplicate_acks == 3) {
      
      VLOG(INFO, "3 duplicate ACKs happened.");
      // Stopping the timer
      stop_timer();
      // Resending the first packet in the window
      resend_packets(duplicate_acks);
      // Restarting the timer
      start_timer();

      duplicate_acks = 0;
    }
  }
  // Else acknowledge the packet and reset the duplicate acknowledgement counter
  else {
    duplicate_acks = 0;
  }
  // Decrementing the number of unacknowledged packets by the shifted number of
  // packets
  unacknowledged_packets -= i;
  if (unacknowledged_packets > 0) {
    // If there are still unacknowledged packets, restart the timer
    start_timer();
  }
}

// Initialize the sent_packets array
void init_sent_packets() {
  for (int i = 0; i < WINDOW_SIZE; i++) {
    sent_packets[i] = NULL;
  }
}

// Add a packet to the sent_packets array
void add_packet_to_window(tcp_packet *pkt) {
  for (int i = 0; i < WINDOW_SIZE; i++) {
    // If the slot is empty, add the packet to the slot
    if (sent_packets[i] == NULL) {
      sent_packets[i] = pkt;
      break;
      // break out of the loop
    }
  }
}

// Main function
int main(int argc, char **argv) {
  int portno, len;        // Port number and length of the packet
  char *hostname;         // Hostname
  char buffer[DATA_SIZE]; // Buffer to store the data
  FILE *fp;               // File pointer
  // Checking the number of arguments
  if (argc != 4) {
    fprintf(stderr, "usage: %s <hostname> <port> <FILE>\n", argv[0]);
    // Exiting the program
    exit(0);
  }
  // Assigning the hostname, port number and file pointer
  hostname = argv[1];
  portno = atoi(argv[2]);
  // Opening the file in the read binary mode
  fp = fopen(argv[3], "rb");
  // Checking if the file is opened successfully
  if (fp == NULL) {
    error(argv[3]);
  }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");
  // Zeroing the server address
  bzero((char *)&serveraddr, sizeof(serveraddr));
  // Setting the server address
  serverlen = sizeof(serveraddr);
  // Checking if the hostname is valid
  if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
    // If not, print an error message and exit
    fprintf(stderr, "ERROR, invalid host %s\n", hostname);

    exit(0);
  }
  // Setting the server address
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_port = htons(portno); // Setting the port number
  // Initializing the timer
  init_timer(RETRY, resend_packets);
  // Initializing the sent_packets array
  init_sent_packets();
  // Initializing the end of file flag
  int eof_reached = 0;

  while (1) {

    // If unacknowledged packets is equal to the window size or the end of file
    // is reached, break
    while ((unacknowledged_packets < WINDOW_SIZE) && (eof_reached == 0)) {

      len = fread(buffer, 1, DATA_SIZE, fp);
      // If the length of the data read is 0, set the end of file flag to 1 and
      // make a packet with no data
      if (len <= 0) {
        // Printing the end of file message
        VLOG(INFO, "End Of File has been reached");
        // Making a packet with no data
        tcp_packet *sndpkt = make_packet(0);
        // Setting the sequence number of the packet
        sndpkt->hdr.seqno = next_seqno; // Starting byte of the packet
        if (sent_packets[0] == NULL) {
          // If the packet is the first packet in the sent_packets array, start
          // the timer
          start_timer();
        }
        // Adding the packet to the sent_packets array
        add_packet_to_window(sndpkt);
        // Sending the packet
        send_packet(sndpkt);
        // Updating the sequence number of the next packet
        next_seqno = next_seqno + len;
        // Incrementing the number of unacknowledged packets
        unacknowledged_packets++;
        // Setting the end of file flag to 1
        eof_reached = 1;
      }
      // Else make a packet with the data read and send it
      else {
        tcp_packet *sndpkt = make_packet(len);
        // Copying the data read to the packet
        memcpy(sndpkt->data, buffer, len);
        // Setting the sequence number of the packet
        sndpkt->hdr.seqno = next_seqno; // Starting byte of the packet
        if (sent_packets[0] == NULL) {
          start_timer();
        }
        add_packet_to_window(sndpkt);
        // Sending the packet
        send_packet(sndpkt);
        // Updating the sequence number of the next packet
        next_seqno = next_seqno + len;
        // Incrementing the number of unacknowledged packets
        unacknowledged_packets++;
        slide_end++;
      }
    }

    wait_ack();
    // If the end of file is reached and there are no unacknowledged packets,
    // print a message and exit
    if (eof_reached == 1 && unacknowledged_packets == 0) {
      printf("End of file reached and acknowledgement received. Exiting the "
             "program\n");
      // Closing the file
      fclose(fp);
      // Closing the socket
      close(sockfd);
      // Exiting the program
      break;
    }
  }

  // Closing the file
  fclose(fp);
  // Closing the socket
  close(sockfd);
  // Exiting the program
  return 0;
}