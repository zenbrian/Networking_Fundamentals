#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ADDR_LEN 6
#define ETH_HEADER_LEN 14

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD

struct ethernet_hdr {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed));

void ethernet_print_mac(const uint8_t *mac);
void ethernet_print_header(const struct ethernet_hdr *hdr);

#endif
