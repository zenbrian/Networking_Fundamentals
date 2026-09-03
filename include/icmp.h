#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

#define ICMP_TIME_EXCEEDED 11

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

void icmp_print_header(const struct icmp_hdr *icmp, size_t length);
int icmp_verify_checksum(const void *icmp, size_t length);
void icmp_handle(uint8_t *packet, size_t length);

void icmp_receive(int fd, const uint8_t *frame, size_t len);

// 新增：發送 ICMP Time Exceeded 回覆
void icmp_send_time_exceeded(int fd, const uint8_t *orig_frame, size_t orig_len);

#endif
