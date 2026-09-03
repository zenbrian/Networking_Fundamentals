#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netpacket/packet.h>

#include "config.h"
#include "ethernet.h"
#include "ipv4.h"
#include "icmp.h"
#include "checksum.h"

int main()
{
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex("tap0");
    sll.sll_protocol = htons(ETH_P_ALL);

    if (sll.sll_ifindex == 0) {
        perror("if_nametoindex tap0 (請確保 ./network 正在執行中！)");
        close(sockfd);
        return 1;
    }

    unsigned char frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)];
    memset(frame, 0, sizeof(frame));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HEADER_LEN);
    struct icmp_hdr *icmp = (struct icmp_hdr *)(frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    uint8_t sender_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

    // 1. Ethernet Header (14 bytes)
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 2. IPv4 Header (20 bytes)
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr));
    ip->identification = htons(1);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_ICMP; // 1
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 3. ICMP Header (8 bytes)
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons(1234);
    icmp->sequence = htons(1);
    icmp->checksum = ipv4_checksum(icmp, sizeof(struct icmp_hdr));

    int len = sizeof(frame);

    printf("Sending ICMP Echo Request via Raw Socket: 10.0.0.1 -> 10.0.0.2 (ID: 1234, Seq: 1)...\n");

    if (sendto(sockfd, frame, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto");
    } else {
        printf("Sent %d bytes ICMP frame successfully!\n", len);
    }

    close(sockfd);
    return 0;
}
