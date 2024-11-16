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

#define MAX_BUFFER_SIZE 10 // Buffer for out-of-order packets

// Array to store the receiver buffer packets

tcp_packet *receiver_buffer_packet[MAX_BUFFER_SIZE];

int next_expected_seqno = 0; // The next expected sequence number

int next_expected_packet_number = 0; // The next expected packet number

// Initializing the receiver buffer packet to NULL

void init_receiver_buffer_packet() {

  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    receiver_buffer_packet[i] = NULL;
  }
}

void send_ack(tcp_packet *recvpkt, int sockfd, struct sockaddr_in *clientaddr,

              int clientlen) {

  tcp_packet *ackpkt = make_packet(0); // Empty packet for ACK

  ackpkt->hdr.ctr_flags = ACK;

  ackpkt->hdr.ackno = next_expected_seqno;

  // Final acknowledgement of the end of the file received.

  if (recvpkt->hdr.data_size == 0) {

    printf("Sending ack for the end of file packet received\n");

    ackpkt->hdr.ackno = -1;
  }

  printf("Sending ACK %d\n", ackpkt->hdr.ackno);

  if (sendto(sockfd, ackpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)clientaddr,

             clientlen) < 0) {

    error("ERROR in sendto");
  }

  // free(ackpkt);
}

void write_to_file(tcp_packet *rcvpkt, FILE *fp) {

  printf("Packet size : %d, Packet seqno: %d written to file\n",

         rcvpkt->hdr.data_size, rcvpkt->hdr.seqno);

  fwrite(rcvpkt->data, 1, rcvpkt->hdr.data_size, fp);

  fflush(fp);
}

void print_buffer() {

  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    if (receiver_buffer_packet[i] == NULL) {

      printf("NULL\n");

      continue;
    }

    printf("Packet seqno: %d\n", receiver_buffer_packet[i]->hdr.seqno);
  }
}

// does insertion sorting to add packets in order

void buffer_packet(tcp_packet *rcvpkt) {

  if (rcvpkt->hdr.seqno < next_expected_seqno) {

    return;
  }

  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    if (receiver_buffer_packet[i] != NULL &&

        receiver_buffer_packet[i]->hdr.seqno == rcvpkt->hdr.seqno) {

      return;
    }
  }

  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    if (receiver_buffer_packet[i] == NULL ||

        receiver_buffer_packet[i]->hdr.seqno > rcvpkt->hdr.seqno) {

      // Shift everything right starting from this position

      for (int j = MAX_BUFFER_SIZE - 1; j > i; j--) {

        if (receiver_buffer_packet[j - 1] != NULL) {

          receiver_buffer_packet[j] = receiver_buffer_packet[j - 1];
        }
      }

      // Create deep copy of the packet

      // tcp_packet *new_packet = malloc(sizeof(tcp_packet));

      // if (new_packet == NULL) {

      //   error("Failed to allocate memory for packet");

      // }

      // memcpy(new_packet, rcvpkt, sizeof(tcp_packet));

      // receiver_buffer_packet[i] = new_packet;

      receiver_buffer_packet[i] = rcvpkt;

      break;
    }
  }

  print_buffer();
}

void manage_buffered_packet(tcp_packet *rcvpkt, int sockfd,

                            struct sockaddr_in *clientaddr, int clientlen,

                            FILE *fp) {

  int shift = 0;

  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    if (receiver_buffer_packet[i] == NULL ||

        receiver_buffer_packet[i]->hdr.seqno != next_expected_seqno) {

      break;

    }

    else {

      shift++;

      next_expected_seqno += receiver_buffer_packet[i]->hdr.data_size;

      // Write to the end of the file

      fwrite(receiver_buffer_packet[i]->data, 1,

             receiver_buffer_packet[i]->hdr.data_size, fp);

      printf("Packet seqno: %d written to file\n",

             receiver_buffer_packet[i]->hdr.seqno);

      fflush(fp);
    }
  }

  // left shift by shift packets

  for (int i = shift; i < MAX_BUFFER_SIZE; i++) {

    receiver_buffer_packet[i - shift] = receiver_buffer_packet[i];
  }

  for (int i = MAX_BUFFER_SIZE - shift; i < MAX_BUFFER_SIZE; i++) {

    receiver_buffer_packet[i] = NULL;
  }

  // remove all the starting NULL packet
}

void process_packet(tcp_packet *rcvpkt, FILE *fp, int sockfd,

                    struct sockaddr_in *clientaddr, int clientlen) {

  // Receive packet is greater than the next expected sequence number

  if (rcvpkt->hdr.seqno > next_expected_seqno) {

    // Write to the buffer

    buffer_packet(rcvpkt);

  } else if (rcvpkt->hdr.seqno < next_expected_seqno) {

    printf("Packet already received\n");

    // manage_buffered_packet(rcvpkt, sockfd, clientaddr, clientlen, fp);

  } else {

    buffer_packet(rcvpkt);

    manage_buffered_packet(rcvpkt, sockfd, clientaddr, clientlen, fp);
  }

  send_ack(rcvpkt, sockfd, clientaddr, clientlen);
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

  init_receiver_buffer_packet(); // Initialize receiver buffer

  // Main loop: wait for incoming packets

  while (1) {

    // Receive UDP datagram from client

    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr,

                 (socklen_t *)&clientlen) < 0) {

      error("ERROR in recvfrom");
    }

    recvpkt = (tcp_packet *)buffer;

    printf("Received packet %d with data size %d. Expected: %d\n",

           recvpkt->hdr.seqno, recvpkt->hdr.data_size, next_expected_seqno);

    assert(get_data_size(recvpkt) <= DATA_SIZE);

    // Check for end of file packet (with data size 0)

    if (recvpkt->hdr.data_size == 0 && receiver_buffer_packet[0] == NULL) {

      VLOG(INFO, "End of file has been reached.");

      // Send the acknowledgement to the sender that the file has been received

      buffer_packet(recvpkt);

      manage_buffered_packet(recvpkt, sockfd, &clientaddr, clientlen, fp);

      send_ack(recvpkt, sockfd, &clientaddr, clientlen);

      // fclose(fp);

      // close(sockfd);

      // break;
    }

    process_packet(recvpkt, fp, sockfd, &clientaddr, clientlen);
  }

  return 0;
}
