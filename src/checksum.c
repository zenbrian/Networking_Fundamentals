#include <stdint.h>
#include "checksum.h"

uint16_t ipv4_checksum(
    const void *data,
    int length
)
{
    const uint16_t *ptr = data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length == 1) {
        sum += *((const uint8_t *)ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

#include <string.h>
#include <arpa/inet.h>

struct tcp_pseudo_hdr {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_len;
} __attribute__((packed));

uint16_t tcp_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    const void *tcp_data,
    int tcp_len
)
{
    struct tcp_pseudo_hdr pseudo;
    pseudo.src_ip = src_ip;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = 6; // IPPROTO_TCP
    pseudo.tcp_len = htons((uint16_t)tcp_len);

    int total_len = sizeof(pseudo) + tcp_len;
    uint8_t buf[total_len];
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), tcp_data, tcp_len);

    return ipv4_checksum(buf, total_len);
}
