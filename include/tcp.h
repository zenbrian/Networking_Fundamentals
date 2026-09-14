#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* IP Protocol 號碼 */
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

/* TCP Flags 定義 */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP Header 結構 (RFC 793 - 固定最小 20 Bytes) */
struct tcp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;

    uint32_t seq;
    uint32_t ack;

    uint8_t data_offset;
    uint8_t flags; 

    uint16_t window;

    uint16_t checksum;

    uint16_t urgent_ptr;
} __attribute__((packed));

enum tcp_state
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED
};

struct tcp_socket
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    enum tcp_state state;
};

/* 印出 TCP 標頭資訊 */
void tcp_print_header(const struct tcp_hdr *tcp);
void tcp_receive(int fd, const uint8_t *buffer, size_t len);

void tcp_init(void);
int tcp_listen(uint16_t port);
struct tcp_socket* tcp_find_listener(uint16_t port);
void tcp_dump_table(void);

int tcp_send_syn_ack(int fd, struct tcp_socket *conn);

#endif /* TCP_H */
