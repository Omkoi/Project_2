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


#define MAX_BUFFER_SIZE 20 // Buffer for out-of-order packets

//Array to store the receiver buffer packets
tcp_packet *receiver_buffer_packet[MAX_BUFFER_SIZE];
int next_expected_seqno = 0;         // The next expected sequence number
int next_expected_packet_number = 0; // The next expected packet number


//Initializing the receiver buffer packet to NULL
void init_receiver_buffer_packet() {
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    receiver_buffer_packet[i] = NULL;
  }
}

void send_ack(tcp_packet *recvpkt, int sockfd, struct sockaddr_in *clientaddr,
              int clientlen) {


  
  tcp_packet *ackpkt = make_packet(0); // Empty packet for ACK
  ackpkt->hdr.ctr_flags = ACK;

  // Final acknowledgement of the end of the file received.
  if (recvpkt->hdr.data_size == 0) {
    printf("Sending ack for the end of file packet received\n");
    ackpkt->hdr.ackno = 0;
  }

  //Indicating the duplicate acknowledgement
  else if (recvpkt->hdr.seqno > next_expected_seqno) {
    ackpkt->hdr.ackno = -1;
  }

  else if (next_expected_seqno > recvpkt->hdr.seqno) {
    ackpkt->hdr.ackno = next_expected_seqno;
  }

  else {
    next_expected_seqno = next_expected_seqno + recvpkt->hdr.data_size; //Increasing the next expected sequence number
    next_expected_packet_number++;
    ackpkt->hdr.ackno = next_expected_seqno;     // Next acknowledgement number is the next expected sequence number
    }                 // Set the ACK flag


  printf("Sending ACK %d\n", ackpkt->hdr.ackno);

  if (sendto(sockfd, ackpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)clientaddr,
             clientlen) < 0) {
    error("ERROR in sendto");
  }
  free(ackpkt);
}



void write_to_file(tcp_packet *rcvpkt, FILE *fp) {
  printf("Packet size : %d, Packet number: %d, Packet seqno: %d written to file\n", rcvpkt->hdr.data_size, rcvpkt->hdr.packet_number, rcvpkt->hdr.seqno);
  fwrite(rcvpkt->data, 1, rcvpkt->hdr.data_size, fp);
  fflush(fp);
  
}



void buffer_packet(tcp_packet *rcvpkt) {
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {

    if (receiver_buffer_packet[i] == NULL) {
      receiver_buffer_packet[i] = rcvpkt;
      break;
    }
  }
}


void manage_buffered_packet(tcp_packet *rcvpkt, int sockfd, struct sockaddr_in *clientaddr, int clientlen, FILE *fp) {

  //Go through the whole list of buffered packets
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    if (receiver_buffer_packet[i] == NULL) {
      continue;
    }
    else {
      if (receiver_buffer_packet[i]->hdr.seqno == next_expected_seqno) {
        send_ack(rcvpkt, sockfd, clientaddr,  clientlen);
        write_to_file(receiver_buffer_packet[i], fp);
        printf("Written from buffer \n");
        free(receiver_buffer_packet[i]);
        receiver_buffer_packet[i] = NULL;
      }
    }
  }
}

  

void process_packet(tcp_packet *rcvpkt, FILE *fp, int sockfd,
                    struct sockaddr_in *clientaddr, int clientlen) {
  if (rcvpkt->hdr.seqno == next_expected_seqno) {
    send_ack(rcvpkt, sockfd, clientaddr, clientlen);     // Send an acknowledgement to the sender that the packet has been received
    write_to_file(rcvpkt, fp);     // If the packet is in order, write it to the file
    manage_buffered_packet(rcvpkt, sockfd, clientaddr, clientlen, fp);  // Process any buffered packets that can now be processed
  }
  else if (rcvpkt->hdr.seqno > next_expected_seqno) {
    // If the packet is out of order, buffer it
    printf("Out of order packet with packet number and packet seqno: %d, %d\n",
           rcvpkt->hdr.packet_number, rcvpkt->hdr.seqno);
    buffer_packet(rcvpkt);
    send_ack(rcvpkt, sockfd, clientaddr, clientlen);
  } else {
    printf(" Expected sequence number is greater than the received packet "
           "sequence number. Packet already received\n");
    send_ack(rcvpkt, sockfd, clientaddr, clientlen);
  }
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

  VLOG(DEBUG, "epoch time, bytes received, sequence number")

  // Main loop: wait for incoming packets
  while (1) {
    // Receive UDP datagram from client
    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr,
                 (socklen_t *)&clientlen) < 0) {
      error("ERROR in recvfrom");
    }
    recvpkt = (tcp_packet *)buffer;

    printf("Received packet %d with data size %d. Expected: %d\n", recvpkt->hdr.seqno, recvpkt->hdr.data_size, next_expected_seqno);
          

    assert(get_data_size(recvpkt) <= DATA_SIZE);
    // Check for end of file packet (with data size 0)
    if (recvpkt->hdr.data_size == 0) {
      VLOG(INFO, "End of file has been reached.");
      //Send the acknowledgement to the sender that the file has been received
      send_ack(recvpkt, sockfd, &clientaddr, clientlen);
      fclose(fp);
      close(sockfd);
      break;
    }
    process_packet(recvpkt, fp, sockfd, &clientaddr, clientlen);
  }
  return 0;
}
