#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>

#define UDP_HEADER_LEN 8
#define MAX_UDP_SOCKETS 16

/* UDP Header (RFC 768) - 固定 8 Bytes */
struct udp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

typedef void (*udp_handler_t)(const uint8_t *data, size_t len);

/* 簡單的 UDP Socket 結構 */
struct udp_socket
{
    uint16_t port;                  // 監聽的 Port
    udp_handler_t handler;          // 要做什麼
};

/* 函式宣告 */
void udp_init(void);
int udp_bind(uint16_t port, udp_handler_t handler);
struct udp_socket *udp_lookup(uint16_t port);
void udp_print_header(const struct udp_hdr *udp);
void udp_receive(int fd, const uint8_t *buffer, size_t len);
int udp_send(int fd, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *data, size_t len);

#endif /* UDP_H */
