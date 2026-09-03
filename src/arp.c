#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "arp.h"

void arp_build_request(
    struct arp_packet *arp,
    const uint8_t *src_mac,
    const uint8_t *src_ip,
    const uint8_t *target_ip
)
{
    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen = 6;
    arp->plen = 4;
    arp->oper = htons(ARP_REQUEST);

    memcpy(arp->sha, src_mac, 6);
    memcpy(arp->spa, src_ip, 4);
    memset(arp->tha, 0x00, 6);
    memcpy(arp->tpa, target_ip, 4);
}
