#include <stdio.h>
#include <arpa/inet.h>
#include "ipv4.h"

void ipv4_print_ip(uint32_t ip)
{
    uint32_t host_ip = ntohl(ip);
    printf("%u.%u.%u.%u",
           (host_ip >> 24) & 0xFF,
           (host_ip >> 16) & 0xFF,
           (host_ip >> 8) & 0xFF,
           host_ip & 0xFF);
}

void ipv4_print_header(const struct ipv4_hdr *ip)
{
    uint8_t version = ip->version_ihl >> 4;
    uint8_t ihl = ip->version_ihl & 0x0F;

    printf("\nIPv4 Packet\n");
    printf("------------------\n");
    printf("Version       : %u\n", version);
    printf("Header Length : %u bytes\n", ihl * 4);
    printf("TTL           : %u\n", ip->ttl);
    printf("Protocol      : %u\n", ip->protocol);
    printf("Source IP     : ");
    ipv4_print_ip(ip->src_ip);
    printf("\nDestination IP: ");
    ipv4_print_ip(ip->dst_ip);
    printf("\n\n");
}
