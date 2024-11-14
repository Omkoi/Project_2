#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <assert.h>

#include "packet.h"
#include "common.h"

#define RETRY  120 // milliseconds

#define WINDOW_SIZE 10
#define MAX_SEQ_NUM 4294967295U // Maximum value for a 32-bit unsigned integer

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer; 
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;       
FILE *fp;

uint32_t send_base = 0;
uint32_t next_seqno = 0;
int dup_ack_count = 0;
int end_of_file = 0;

void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // Resend the packet with the smallest sequence number in window
        VLOG(INFO, "Timeout occurred, resending packet %u", send_base);

        char buffer[DATA_SIZE];
        int len;

        fseek(fp, send_base, SEEK_SET);
        len = fread(buffer, 1, DATA_SIZE, fp);
        if (len <= 0) {
            VLOG(INFO, "End of file reached when trying to retransmit packet %u", send_base);
            return;
        }
        sndpkt = make_packet(len);
        memcpy(sndpkt->data, buffer, len);
        sndpkt->hdr.seqno = send_base;
        sndpkt->hdr.data_size = len;

        if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                (const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }

        free(sndpkt);

        // Restart timer
        start_timer();
    }
}

void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

void init_timer(int delay, void (*sig_handler)(int)) 
{
    signal(SIGALRM, sig_handler);
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_usec = 0;  
    timer.it_value.tv_sec = delay / 1000;       // Initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}

/* Sequence number comparison considering wrap around */
int seq_num_less(uint32_t seq1, uint32_t seq2)
{
    return ((int32_t)(seq1 - seq2) < 0);
}

int main (int argc, char **argv)
{
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];

    /* Check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* Socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) 
        error("ERROR opening socket");

    /* Initialize server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* Convert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* Build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    /* Initialize timer */
    init_timer(RETRY, resend_packets);

    while (1)
    {
        // Send packets while window is not full and there is data to send
        while (((next_seqno - send_base + MAX_SEQ_NUM) % MAX_SEQ_NUM) < WINDOW_SIZE * DATA_SIZE && !end_of_file) {

            // Read data from file at position next_seqno
            fseek(fp, next_seqno, SEEK_SET);
            len = fread(buffer, 1, DATA_SIZE, fp);
            if (len <= 0) {
                // No more data to read
                end_of_file = 1;
                break;
            }

            // Create packet
            sndpkt = make_packet(len);
            memcpy(sndpkt->data, buffer, len);
            sndpkt->hdr.seqno = next_seqno;
            sndpkt->hdr.data_size = len;

            // Send packet
            VLOG(DEBUG, "Sending packet %u to %s", next_seqno, inet_ntoa(serveraddr.sin_addr));
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                    (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            if (send_base == next_seqno) {
                // Start timer
                start_timer();
            }

            next_seqno = (next_seqno + len) % MAX_SEQ_NUM;

            free(sndpkt); // Packet can be regenerated from the file
        }

        if (send_base == next_seqno && end_of_file) {
            // All packets sent and acknowledged
            // Send an empty packet to indicate end-of-file
            sndpkt = make_packet(0);
            sndpkt->hdr.seqno = next_seqno;
            sndpkt->hdr.data_size = 0;

            VLOG(DEBUG, "Sending end-of-file packet %u", next_seqno);
            if(sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                    (const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            free(sndpkt);

            // Wait for final ACK
            char ackbuf[MSS_SIZE];
            struct sockaddr_in ack_addr;
            socklen_t ack_len = sizeof(ack_addr);
            int recvlen = recvfrom(sockfd, ackbuf, MSS_SIZE, 0,
                                   (struct sockaddr *) &ack_addr, &ack_len);
            if (recvlen < 0) {
                perror("recvfrom");
                continue;
            }

            recvpkt = (tcp_packet *)ackbuf;
            assert(get_data_size(recvpkt) <= DATA_SIZE);

            uint32_t ackno = recvpkt->hdr.ackno;

            VLOG(DEBUG, "Received final ACK %u", ackno);

            if (ackno == next_seqno) {
                VLOG(INFO, "File transfer completed successfully.");
                break;
            } else {
                VLOG(WARNING, "Unexpected ACK %u received, expecting %u", ackno, next_seqno);
            }
        }

        // Wait for ACKs
        // Blocking recvfrom()
        char ackbuf[MSS_SIZE];
        struct sockaddr_in ack_addr;
        socklen_t ack_len = sizeof(ack_addr);
        int recvlen = recvfrom(sockfd, ackbuf, MSS_SIZE, 0,
                               (struct sockaddr *) &ack_addr, &ack_len);
        if (recvlen < 0) {
            perror("recvfrom");
            continue;
        }

        recvpkt = (tcp_packet *)ackbuf;
        assert(get_data_size(recvpkt) <= DATA_SIZE);

        uint32_t ackno = recvpkt->hdr.ackno;

        VLOG(DEBUG, "Received ACK %u", ackno);

        if (seq_num_less(send_base, ackno) && seq_num_less(ackno, next_seqno + DATA_SIZE)) {
            // New ACK received
            send_base = ackno;
            dup_ack_count = 0;

            if (send_base == next_seqno && end_of_file) {
                // All packets acknowledged
                stop_timer();
                // Continue to send end-of-file packet if not already sent
                continue;
            } else {
                // Restart timer
                stop_timer();
                if (send_base != next_seqno) {
                    start_timer();
                }
            }
        } else if (ackno == send_base) {
            // Duplicate ACK
            dup_ack_count++;
            if (dup_ack_count == 3) {
                // Resend packet with smallest seqno in window (send_base)
                fseek(fp, send_base, SEEK_SET);
                len = fread(buffer, 1, DATA_SIZE, fp);
                if (len <= 0) {
                    VLOG(INFO, "End of file reached when trying to retransmit packet %u", send_base);
                    continue;
                }
                sndpkt = make_packet(len);
                memcpy(sndpkt->data, buffer, len);
                sndpkt->hdr.seqno = send_base;
                sndpkt->hdr.data_size = len;

                VLOG(INFO, "Resending packet %u due to 3 duplicate ACKs", send_base);
                if(sendto(sockfd, sndpkt, TCP_HDR_SIZE + get_data_size(sndpkt), 0,
                        (const struct sockaddr *)&serveraddr, serverlen) < 0)
                {
                    error("sendto");
                }

                free(sndpkt);
                dup_ack_count = 0;
            }
        }

        // If timer expired, resend_packets() will handle retransmission
    }

    fclose(fp);
    return 0;
}


