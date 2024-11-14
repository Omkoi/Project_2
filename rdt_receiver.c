#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 10
#define MAX_SEQ_NUM 4294967296U // 2^32

tcp_packet *recvpkt;
tcp_packet *sndpkt;

typedef struct {
    uint32_t seqno;
    int data_size;
    char data[DATA_SIZE];
} packet_t;

packet_t window[WINDOW_SIZE];
int window_count = 0;
uint32_t expected_seqno = 0;

void insert_packet(packet_t *pkt) {
    // Check if packet is already in the window
    for (int i = 0; i < window_count; i++) {
        if (window[i].seqno == pkt->seqno) {
            // Already have this packet
            return;
        }
    }
    if (window_count < WINDOW_SIZE) {
        window[window_count++] = *pkt;
        VLOG(DEBUG, "Stored packet %u in window", pkt->seqno);
    } else {
        // Window is full; ignore packet or handle overflow
        VLOG(WARNING, "Window full, cannot store packet %u", pkt->seqno);
    }
}

void deliver_packets(FILE *fp) {
    int delivered = 1;
    while (delivered) {
        delivered = 0;
        for (int i = 0; i < window_count; i++) {
            if (window[i].seqno == expected_seqno) {
                // Deliver packet
                fseek(fp, expected_seqno, SEEK_SET);
                fwrite(window[i].data, 1, window[i].data_size, fp);
                VLOG(DEBUG, "Delivered packet %u", expected_seqno);

                expected_seqno = (expected_seqno + window[i].data_size) % MAX_SEQ_NUM;

                // Remove packet from window
                window[i] = window[window_count - 1];
                window_count--;
                delivered = 1;
                break; // Restart the loop
            }
        }
    }
}

int is_in_window(uint32_t seqno) {
    uint32_t distance = (seqno - expected_seqno + MAX_SEQ_NUM) % MAX_SEQ_NUM;
    return distance < WINDOW_SIZE * DATA_SIZE;
}

int main(int argc, char **argv) {
    int sockfd; /* socket */
    int portno; /* port to listen on */
    socklen_t clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    int optval; /* flag value for setsockopt */
    FILE *fp;
    char buffer[MSS_SIZE];

    /* 
     * Check command line arguments 
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /* 
     * Socket: create the parent socket 
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* Setsockopt: eliminate "ERROR on binding: Address already in use" error. */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, 
            (const void *)&optval , sizeof(int));

    /*
     * Build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /* 
     * Bind: associate the parent socket with a port 
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr, 
                sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* 
     * Main loop: wait for a datagram, then process it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    clientlen = sizeof(clientaddr);
    while (1) {
        /*
         * recvfrom: receive a UDP datagram from a client
         */
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, &clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        uint32_t seqno = recvpkt->hdr.seqno;
        int data_size = recvpkt->hdr.data_size;

        // Process end-of-file packet immediately
        if (data_size == 0) {
            // End of file
            VLOG(INFO, "End of file reached");
            fclose(fp);

            // Send final ACK
            sndpkt = make_packet(0);
            sndpkt->hdr.ackno = expected_seqno;
            sndpkt->hdr.ctr_flags = ACK;
            VLOG(DEBUG, "Sending final ACK %u", expected_seqno);
            if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                    (struct sockaddr *) &clientaddr, clientlen) < 0) {
                error("ERROR in sendto");
            }

            break;
        }

        if (is_in_window(seqno)) {
            // Packet is within the window
            packet_t pkt;
            pkt.seqno = seqno;
            pkt.data_size = data_size;
            memcpy(pkt.data, recvpkt->data, data_size);
            insert_packet(&pkt);
            deliver_packets(fp);
        } else {
            // Packet is outside the window, ignore
            VLOG(INFO, "Packet %u outside window [%u, %u), ignored", seqno, expected_seqno, (expected_seqno + WINDOW_SIZE * DATA_SIZE) % MAX_SEQ_NUM);
        }

        // Send cumulative ACK
        sndpkt = make_packet(0);
        sndpkt->hdr.ackno = expected_seqno;
        sndpkt->hdr.ctr_flags = ACK;
        VLOG(DEBUG, "Sending ACK %u", expected_seqno);
        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, 
                (struct sockaddr *) &clientaddr, clientlen) < 0) {
            error("ERROR in sendto");
        }
    }

    return 0;
}


