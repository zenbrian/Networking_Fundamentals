#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ADDR_LEN 6
#define ETH_HEADER_LEN 14

#ifndef ETHERTYPE_IPV4
#define ETHERTYPE_IPV4 0x0800
#endif
#ifndef ETHERTYPE_ARP
#define ETHERTYPE_ARP  0x0806
#endif
#ifndef ETHERTYPE_IPV6
#define ETHERTYPE_IPV6 0x86DD
#endif

struct ethernet_hdr {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed));

void ethernet_print_mac(const uint8_t *mac);
void ethernet_print_header(const struct ethernet_hdr *hdr);

int ethernet_is_broadcast(const uint8_t *mac);
int ethernet_is_for_me(const uint8_t *mac);
int ethernet_accept_frame(const struct ethernet_hdr *hdr);

#endif
