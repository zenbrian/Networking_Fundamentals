#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "config.h"
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

int ethernet_is_broadcast(const uint8_t *mac)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++) {
        if (mac[i] != 0xff)
            return 0;
    }

    return 1;
}

int ethernet_is_for_me(const uint8_t *mac)
{
    return memcmp(
        mac,
        LOCAL_MAC,
        ETH_ADDR_LEN
    ) == 0;
}

int ethernet_accept_frame(
    const struct ethernet_hdr *hdr)
{
    if (ethernet_is_broadcast(hdr->dst)) {
        return 1;
    }

    if (ethernet_is_for_me(hdr->dst)) {
        return 1;
    }

    return 0;
}
