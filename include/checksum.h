#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdint.h>

uint16_t ipv4_checksum(
    const void *data,
    int length
);

#endif
