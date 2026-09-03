#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "config.h"
#include "ethernet.h"
#include "arp.h"
#include "arp_table.h"

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

void arp_handle_reply(const struct arp_packet *arp)
{
    arp_table_insert(arp->spa, arp->sha);
    printf("[ARP] Reply Received\n");
    fflush(stdout);
    arp_table_dump();
}

void arp_handle_request(int fd, const struct arp_packet *arp)
{
    // 只回應詢問 LOCAL_IP (10.0.0.2) 的 ARP Request
    if (memcmp(arp->tpa, LOCAL_IP, 4) != 0) {
        return;
    }

    printf("[ARP] Request for %u.%u.%u.%u received -> Generating ARP Reply\n",
           arp->tpa[0], arp->tpa[1], arp->tpa[2], arp->tpa[3]);
    fflush(stdout);

    // 順便學習請求者的 IP/MAC
    arp_table_insert(arp->spa, arp->sha);

    // 組裝完整的 Ethernet + ARP Reply Frame (14 + 28 = 42 bytes)
    uint8_t frame[ETH_HEADER_LEN + sizeof(struct arp_packet)];
    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct arp_packet *reply = (struct arp_packet *)(frame + ETH_HEADER_LEN);

    // 1. Ethernet Header
    memcpy(eth->dst, arp->sha, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_ARP);

    // 2. ARP Header & Payload
    reply->htype = htons(1);
    reply->ptype = htons(0x0800);
    reply->hlen = 6;
    reply->plen = 4;
    reply->oper = htons(ARP_REPLY);

    memcpy(reply->sha, LOCAL_MAC, 6);
    memcpy(reply->spa, LOCAL_IP, 4);
    memcpy(reply->tha, arp->sha, 6);
    memcpy(reply->tpa, arp->spa, 4);

    // 3. 發送回 TAP 虛擬網卡
    ssize_t sent = write(fd, frame, sizeof(frame));
    if (sent < 0) {
        perror("[ARP] write failed");
    } else {
        printf("[ARP] Reply sent (%ld bytes)\n", sent);
        fflush(stdout);
    }
}

void arp_receive(int fd, const uint8_t *payload, size_t len)
{
    if (len < sizeof(struct arp_packet))
        return;

    const struct arp_packet *arp = (const struct arp_packet *)payload;
    uint16_t opcode = ntohs(arp->oper);

    if (opcode == ARP_REQUEST) {
        arp_handle_request(fd, arp);
    } else if (opcode == ARP_REPLY) {
        arp_handle_reply(arp);
    }
}
