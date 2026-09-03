#ifndef IPV4_H
#define IPV4_H

#include <stdint.h>

#define IPPROTO_ICMP 1

struct ipv4_hdr {
    uint8_t  version_ihl;
    uint8_t  tos;

    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;

    uint8_t  ttl;
    uint8_t  protocol;

    uint16_t checksum;

    uint32_t src_ip;
    uint32_t dst_ip;
} __attribute__((packed));

void ipv4_print_ip(uint32_t ip);
void ipv4_print_header(const struct ipv4_hdr *ip);

#endif
