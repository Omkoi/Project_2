enum packet_type {
    DATA,
    ACK,
};

typedef struct {
    int seqno;
    int ackno;
    int ctr_flags;
    int data_size;
    int packet_number;
}tcp_header;

#define WINDOW_SIZE 10

#define MSS_SIZE    1500
#define UDP_HDR_SIZE    8
#define IP_HDR_SIZE    20
#define TCP_HDR_SIZE    sizeof(tcp_header)
#define DATA_SIZE   (MSS_SIZE - TCP_HDR_SIZE - UDP_HDR_SIZE - IP_HDR_SIZE)
typedef struct {
    tcp_header  hdr;
    char    data[0];
}tcp_packet;

tcp_packet* make_packet(int seq);
int get_data_size(tcp_packet *pkt);

typedef struct {
    int is_occupied;
    tcp_packet *pkt;
} receiver_buffer_slot;

