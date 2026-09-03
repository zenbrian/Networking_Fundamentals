#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

void icmp_print_header(const struct icmp_hdr *icmp, size_t length);
int icmp_verify_checksum(const void *icmp, size_t length);

#endif
