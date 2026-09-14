#include <stdio.h>
#include <arpa/inet.h>
#include "ethernet.h"
#include "ipv4.h"

#include "tcp.h"

#define MAX_TCP_SOCKETS 64

static struct tcp_socket tcp_table[MAX_TCP_SOCKETS];

void tcp_init(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        tcp_table[i].state = TCP_CLOSED;
    }
}

int tcp_listen(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_LISTEN;
            tcp_table[i].dst_port = port;
            return 0;
        }
    }
    return -1;
}

struct tcp_socket* tcp_find_listener(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_LISTEN && tcp_table[i].dst_port == port) {
            return &tcp_table[i];
        }
    }
    return NULL;
}

struct tcp_socket* tcp_create_connection(uint32_t src_ip, uint32_t dst_ip,uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_SYN_RECEIVED;
            tcp_table[i].src_ip = src_ip;
            tcp_table[i].dst_ip = dst_ip;
            tcp_table[i].src_port = src_port;
            tcp_table[i].dst_port = dst_port;
            return &tcp_table[i];
        }
    }
    return NULL;
}

void tcp_dump_table(void)
{
    printf("\n=== TCP SOCKET TABLE ===\n");
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED)
            continue; // 空的不用印
        printf("[%02d] State: ", i);
        switch (tcp_table[i].state) {
            case TCP_LISTEN:       printf("LISTEN       "); break;
            case TCP_SYN_SENT:     printf("SYN_SENT     "); break;
            case TCP_SYN_RECEIVED: printf("SYN_RECEIVED "); break;
            case TCP_ESTABLISHED:  printf("ESTABLISHED  "); break;
            default:               printf("UNKNOWN      "); break;
        }
        if (tcp_table[i].state == TCP_LISTEN) {
            printf("| Local Port: %u (*:* -> :%u)\n", tcp_table[i].dst_port, tcp_table[i].dst_port);
        } else {
            char sip_str[INET_ADDRSTRLEN];
            char dip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &tcp_table[i].src_ip, sip_str, sizeof(sip_str));
            inet_ntop(AF_INET, &tcp_table[i].dst_ip, dip_str, sizeof(dip_str));
            printf("| %s:%u -> %s:%u\n",
                   sip_str, tcp_table[i].src_port,
                   dip_str, tcp_table[i].dst_port);
        }
    }
    printf("========================\n\n");
}



void tcp_print_header(const struct tcp_hdr *tcp)
{
    printf("\nTCP Packet\n");
    printf("----------------------------------------\n");

    printf("Source Port      : %u\n", ntohs(tcp->src_port));
    printf("Destination Port : %u\n", ntohs(tcp->dst_port));

    printf("SEQ              : %u\n", ntohl(tcp->seq));
    printf("ACK              : %u\n", ntohl(tcp->ack));

    printf("Data Offset      : %u (Header Length: %u bytes)\n",
           (tcp->data_offset >> 4),
           (tcp->data_offset >> 4) * 4);

    printf("Flags            : 0x%02x [ %s%s%s%s%s%s]\n",
           tcp->flags,
           (tcp->flags & TCP_SYN) ? "SYN " : "",
           (tcp->flags & TCP_ACK) ? "ACK " : "",
           (tcp->flags & TCP_FIN) ? "FIN " : "",
           (tcp->flags & TCP_RST) ? "RST " : "",
           (tcp->flags & TCP_PSH) ? "PSH " : "",
           (tcp->flags & TCP_URG) ? "URG " : "");

    printf("                   SYN = %d, ACK = %d, FIN = %d, RST = %d\n",
           (tcp->flags & TCP_SYN) ? 1 : 0,
           (tcp->flags & TCP_ACK) ? 1 : 0,
           (tcp->flags & TCP_FIN) ? 1 : 0,
           (tcp->flags & TCP_RST) ? 1 : 0);

    printf("Window           : %u\n", ntohs(tcp->window));
    printf("Checksum         : 0x%04x\n", ntohs(tcp->checksum));
    printf("Urgent Pointer   : %u\n", ntohs(tcp->urgent_ptr));
    printf("----------------------------------------\n");
}

void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    (void)fd; // 目前還不需要回覆，避免 unused 警告

    // 取得 IPv4 標頭以計算實際 IP Header 長度
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    // 檢查封包總長度是否足夠容納 TCP Header
    if (len < ETH_HEADER_LEN + ip_hdr_len + sizeof(struct tcp_hdr)) {
        printf("[TCP] Packet too short\n");
        return;
    }

    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + ip_hdr_len);
    tcp_print_header(tcp);

    if (tcp->flags & TCP_SYN) {
        uint16_t dst_port = ntohs(tcp->dst_port);
        uint16_t src_port = ntohs(tcp->src_port);

        struct tcp_socket *listener = tcp_find_listener(dst_port);
        if (listener == NULL) {
            printf("[TCP] Port %u not listening -> DROP\n", dst_port);
            return;
        }
        printf("[TCP] Incoming SYN on Port %u\n", dst_port);
        struct tcp_socket *conn = tcp_create_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] Connection table full!\n");
            return;
        }
        tcp_dump_table();

        return;
    }
}
