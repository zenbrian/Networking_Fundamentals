#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdint.h>

uint16_t ipv4_checksum(
    const void *data,
    int length
);

uint16_t tcp_checksum(
    uint32_t src_ip,
    uint32_t dst_ip,
    const void *tcp_data,
    int tcp_len
);

#endif
