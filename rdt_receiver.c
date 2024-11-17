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
int eof_received = 0; // Flag to check if the end of file has been received

// Initializing the receiver buffer packet window to NULL
void init_receiver_buffer_packet() {
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    receiver_buffer_packet[i] = NULL;
  }
}

void send_ack(tcp_packet *recvpkt, int sockfd, struct sockaddr_in *clientaddr,
int clientlen, FILE *fp) {
  tcp_packet *ackpkt = make_packet(0); // Empty packet for ACK

  ackpkt->hdr.ctr_flags = ACK;

  ackpkt->hdr.ackno = next_expected_seqno; // Setting the ackno to the next expected sequence number

  
  // Final acknowledgement of the end of the file received.
  if (recvpkt->hdr.data_size == 0) {
    eof_received = 1;
    //printf("Sending ack for the end of file packet received\n");
    ackpkt->hdr.ackno = -1; //Setting the ackno to -1 to indicate the end of the file
  }


  
  // Sending the ACK to the sender
  if (sendto(sockfd, ackpkt, TCP_HDR_SIZE, 0, (struct sockaddr *)clientaddr,

             clientlen) < 0) {

    error("ERROR in sendto");
  }
}


// Does insertion sorting to add packets in order
void buffer_packet(tcp_packet *rcvpkt) {

  if (rcvpkt->hdr.seqno < next_expected_seqno) { //If the packet is already received and written to the file.
    return;
  }
  // Checking if the packet is already in the buffer but yet to be written to
  // the file. Duplicate packets should not be added to the buffer. So we
  // return.
  //Loop through the buffer to check if the packet is already in the buffer.
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    if (receiver_buffer_packet[i] != NULL && //if the index is not NULL and the seqno matches
        receiver_buffer_packet[i]->hdr.seqno == rcvpkt->hdr.seqno) {
      return;
    }
  }
  //Loop through the buffer to find the correct position to insert the packet.
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    if (receiver_buffer_packet[i] == NULL || //if the index is NULL or the seqno is greater than the seqno of the packet to be inserted
        receiver_buffer_packet[i]->hdr.seqno > rcvpkt->hdr.seqno) {
      // Shift everything right starting from this position
      for (int j = MAX_BUFFER_SIZE - 1; j > i; j--) {
        if (receiver_buffer_packet[j - 1] != NULL) {
          receiver_buffer_packet[j] = receiver_buffer_packet[j - 1];
        }
      }
      receiver_buffer_packet[i] = rcvpkt; //Inserting the packet in the correct inedex
      break;
    }
  }
}

//Write the ordered packets to the file, manage the buffer and update the next expected sequence number
void manage_buffered_packet(tcp_packet *rcvpkt, int sockfd,

                            struct sockaddr_in *clientaddr, int clientlen,

                            FILE *fp) {
  
  //To find the number of packets to be shifted to the left in the buffer. To know how many packets are to be written to the file based on their seqno.
  int shift = 0;
  for (int i = 0; i < MAX_BUFFER_SIZE; i++) {
    if (receiver_buffer_packet[i] == NULL || //if the index is NULL and the packet is not in order
        receiver_buffer_packet[i]->hdr.seqno != next_expected_seqno) {
      break;
    } else {
      //if the packets are in order
      shift++;
      next_expected_seqno += receiver_buffer_packet[i]->hdr.data_size; //Updating the next expected sequence number
      // Write to the file since the packet is in order
      fwrite(receiver_buffer_packet[i]->data, 1,

             receiver_buffer_packet[i]->hdr.data_size, fp);
      fflush(fp);
    }
  }

  // left shift the receiver buffer by shift packets as they are written to the file since they were in order
  for (int i = shift; i < MAX_BUFFER_SIZE; i++) {
    receiver_buffer_packet[i - shift] = receiver_buffer_packet[i];
  }
  for (int i = MAX_BUFFER_SIZE - shift; i < MAX_BUFFER_SIZE; i++) {
    receiver_buffer_packet[i] = NULL; //Setting the remaining indices to NULL
  }
}

void process_packet(tcp_packet *rcvpkt, FILE *fp, int sockfd,

                    struct sockaddr_in *clientaddr, int clientlen) {

  // Receive packet is greater than the next expected sequence number
  if (rcvpkt->hdr.seqno > next_expected_seqno) {
    // Write to the buffer
    buffer_packet(rcvpkt);
  } else if (rcvpkt->hdr.seqno < next_expected_seqno) {
    // If the packet is already received and written to the file.
    // Do nothing
    ;
  } else {
    //If the packet received is the packet expected by the receiver, i.e. the packet is in order
    buffer_packet(rcvpkt); //Adding the packet to the buffer
    manage_buffered_packet(rcvpkt, sockfd, clientaddr, clientlen, fp); //Manage the buffer and write the packets to the packets which can be in order after this packet.
  }
  //Sending the ACK to the sender
  send_ack(rcvpkt, sockfd, clientaddr, clientlen,fp);
}

int main(int argc, char **argv) {
  int optval; //Socket options
  FILE *fp; //File pointer to write the received data to the file
  char buffer[MSS_SIZE]; //Buffer to store the received data
  tcp_packet *recvpkt; //Pointer to the received packet
  int sockfd, portno, clientlen; //Socket file descriptor, port number and client length
  struct sockaddr_in serveraddr, clientaddr; //Server and client addresses
  struct timeval tp; //Time value
  // Check command line arguments
  if (argc != 3) {
    fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
    exit(1);
  }
  portno = atoi(argv[1]); //Converting the port number to integer

  fp = fopen(argv[2], "wb"); //Opening the file to write the received data to in binary mode

  if (fp == NULL) {
    error(argv[2]); //If the file is not found, print an error message and exit
  }
  sockfd = socket(AF_INET, SOCK_DGRAM, 0); //Creating a socket

  if (sockfd < 0) {
    error("ERROR opening socket"); //If the socket is not created, print an error message and exit
  }
  // Set socket options
  optval = 1; //Setting the socket options
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,sizeof(int));

  // Build server's Internet address
  bzero((char *)&serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET; //Setting the family to IPv4
  serveraddr.sin_addr.s_addr = htonl(INADDR_ANY); //Setting the address to any address
  serveraddr.sin_port = htons((unsigned short)portno); //Setting the port number
  // Bind socket to port
  if (bind(sockfd, (struct sockaddr *)&serveraddr, sizeof(serveraddr)) < 0)
    error("ERROR on binding"); //If the socket is not bound to the port, print an error message and exit
  clientlen = sizeof(clientaddr); //Getting the client length


  init_receiver_buffer_packet(); // Initialize receiver buffer

  VLOG(DEBUG, "epoch time, bytes received, sequence number");
  // Main loop
  while (1) {

    // Receive UDP datagram from client
    if (recvfrom(sockfd, buffer, MSS_SIZE, 0, (struct sockaddr *)&clientaddr, (socklen_t *)&clientlen) < 0) {
      error("ERROR in recvfrom"); //If the packet is not received, print an error message and exit
    }

    recvpkt = (tcp_packet *)buffer; //Converting the buffer to a tcp packet

    gettimeofday(&tp, NULL); //Getting the current time 
    VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, get_data_size(recvpkt), recvpkt->hdr.seqno); //Logging the epoch time, bytes received and sequence number

    assert(get_data_size(recvpkt) <= DATA_SIZE); //Asserting that the data size is less than or equal to the maximum data size

    // Check for end of file packet (with data size 0)
    if (recvpkt->hdr.data_size == 0 ) {
      VLOG(INFO, "End of file has been reached."); //Logging that the end of file has been reached
      buffer_packet(recvpkt); //Adding the packet to the buffer
      manage_buffered_packet(recvpkt, sockfd, &clientaddr, clientlen, fp); //Managing the buffer and writing the packets to the file
      send_ack(recvpkt, sockfd, &clientaddr, clientlen, fp); //Sending the ACK to the sender

      // fclose(fp);

      // close(sockfd);

      // break;
    }
    process_packet(recvpkt, fp, sockfd, &clientaddr, clientlen); //Processing the packet
  }
  return 0;
}
