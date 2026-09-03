#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>

#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;

    uint8_t  sha[6];
    uint8_t  spa[4];

    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

void arp_build_request(
    struct arp_packet *arp,
    const uint8_t *src_mac,
    const uint8_t *src_ip,
    const uint8_t *target_ip
);

void arp_handle_reply(const struct arp_packet *arp);
void arp_handle_request(int fd, const struct arp_packet *arp);
void arp_receive(int fd, const uint8_t *payload, size_t len);

#endif
