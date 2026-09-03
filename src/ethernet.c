#include <stdio.h>
#include <arpa/inet.h>
#include "ethernet.h"

void ethernet_print_mac(const uint8_t *mac)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

void ethernet_print_header(
    const struct ethernet_hdr *hdr)
{
    printf("Ethernet Frame\n");
    printf("-------------------------\n");

    printf("Destination : ");
    ethernet_print_mac(hdr->dst);
    printf("\n");

    printf("Source      : ");
    ethernet_print_mac(hdr->src);
    printf("\n");

    printf("EtherType   : 0x%04x\n",
           ntohs(hdr->ethertype));
}
