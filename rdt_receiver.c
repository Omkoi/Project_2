#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 10       // Sliding window size
#define MAX_BUFFER_SIZE 1024 // Buffer for out-of-order packets

// Receiver buffer to store out-of-order packets


receiver_buffer_slot receiver_buffer[MAX_BUFFER_SIZE];
int next_expected_seqno = 0; // The next expected sequence number

// Function to initialize the receiver buffer
void init_receiver_buffer() {
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    receiver_buffer[i].is_occupied = 0;
    receiver_buffer[i].pkt = NULL;
  }
}

// Function to manage buffered packets (process those that are in order)
void manage_buffered_packets(FILE *fp) {
  while (1) {
    int index = next_expected_seqno % MAX_BUFFER_SIZE;
    if (!receiver_buffer[index].is_occupied) {
      break; // No more packets to process
    }
    // Process the packet (write to file and free the buffer slot)
    tcp_packet *pkt = receiver_buffer[index].pkt;
    fseek(fp, pkt->hdr.seqno, SEEK_SET);
    fwrite(pkt->data, 1, pkt->hdr.data_size, fp);
    printf("Processed packet %d with size %d\n", pkt->hdr.seqno,
           pkt->hdr.data_size);

    next_expected_seqno++; // Increment the expected sequence number
    free(pkt);
    receiver_buffer[index].is_occupied = 0; // Mark slot as free
  }
}

// Function to send an acknowledgment for the next expected sequence number
void send_ack(int sockfd, struct sockaddr_in *clientaddr, int clientlen) {
  tcp_packet *ackpkt = make_packet(0); // Empty packet for ACK
  ackpkt->hdr.ackno =
      next_expected_seqno;     // Acknowledge the next expected sequence number
  ackpkt->hdr.ctr_flags = ACK; // Set the ACK flag
  printf("Sending ACK %d\n", ackpkt->hdr.ackno);

  // Send the ACK to the sender
  if (sendto(sockfd, ackpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)clientaddr,
             clientlen) < 0) {
    error("ERROR in sendto");
  }
  free(ackpkt);
}

// Function to process a received packet
void process_packet(tcp_packet *recvpkt, FILE *fp, int sockfd,
                    struct sockaddr_in *clientaddr, int clientlen) {
  if (recvpkt->hdr.seqno == next_expected_seqno) {
    // If the packet is in order, write it to the file
    fwrite(recvpkt->data, 1, recvpkt->hdr.data_size, fp);
    next_expected_seqno =
        recvpkt->hdr.seqno +
        recvpkt->hdr.data_size; // Update expected sequence number

    // Process any buffered packets that can now be processed
    printf("Processing packet %d\n", recvpkt->hdr.seqno);

    manage_buffered_packets(fp);
  } else if (recvpkt->hdr.seqno > next_expected_seqno) {
    // If the packet is out of order, buffer it
    int index = recvpkt->hdr.seqno % MAX_BUFFER_SIZE;
    if (!receiver_buffer[index].is_occupied) {
      printf("Buffering packet %d (out of order)\n", recvpkt->hdr.seqno);

      receiver_buffer[index].pkt =
          malloc(TCP_HDR_SIZE + recvpkt->hdr.data_size);
      memcpy(receiver_buffer[index].pkt, recvpkt,
             TCP_HDR_SIZE + recvpkt->hdr.data_size);
      receiver_buffer[index].is_occupied = 1;
    }
  }

  // Send acknowledgment for the next expected sequence number
  send_ack(sockfd, clientaddr, clientlen);
  printf("Sent ACK %d\n", next_expected_seqno);
}

int main(int argc, char **argv) {
  int optval;
  FILE *fp;
  char buffer[MSS_SIZE];
  tcp_packet *recvpkt;
  int sockfd, portno, clientlen;
  struct sockaddr_in serveraddr, clientaddr;

  // Check command line arguments
  if (argc != 3) {
    fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]);
  fp = fopen(argv[2], "wb");
  if (fp == NULL) {
    error(argv[2]);
  }

  // Create socket
  sockfd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd < 0)
    error("ERROR opening socket");

  // Set socket options
  optval = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
             sizeof(int));

  // Build server's Internet address
  bzero((char *)&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
  serveraddr.sin_port = htons((unsigned short)portno);

  // Bind socket to port
  if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
    error("ERROR on binding");

  clientlen = sizeof(clientaddr);
  init_receiver_buffer(); // Initialize receiver buffer

  VLOG(DEBUG, "Waiting for packets...");

  // Main loop: wait for incoming packets
  while (1) {
    // Receive UDP datagram from client
    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr,
                 (socklen_t *)&clientlen) < 0) {
      error("ERROR in recvfrom");
    }
    recvpkt = (tcp_packet *)buffer;
    printf("Received packet %d. Expected: %d\n", recvpkt->hdr.seqno,
           next_expected_seqno);

    assert(get_data_size(recvpkt) <= DATA_SIZE);

    // Check for end of file packet (with data size 0)
    if (recvpkt->hdr.data_size == 0) {
      VLOG(INFO, "End of file has been reached.");
      fclose(fp);
      break;
    }

    // Process the received packet
    process_packet(recvpkt, fp, sockfd, &clientaddr, clientlen);
  }

  fclose(fp);
  return 0;
}
