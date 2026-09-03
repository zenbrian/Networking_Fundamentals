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
