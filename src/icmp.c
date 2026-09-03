#include <stdio.h>
#include <arpa/inet.h>

#include "icmp.h"
#include "checksum.h"

int icmp_verify_checksum(const void *icmp, size_t length)
{
    uint16_t calc_cksum = ipv4_checksum(icmp, length);
    return (calc_cksum == 0);
}

void icmp_print_header(const struct icmp_hdr *icmp, size_t length)
{
    int is_correct = icmp_verify_checksum(icmp, length);

    printf("\n");
    printf("ICMP Packet\n");
    printf("-------------------\n");
    printf("Type       : %u\n", icmp->type);
    printf("Code       : %u\n", icmp->code);
    printf("Checksum   : 0x%04x (%s)\n", ntohs(icmp->checksum), is_correct ? "OK" : "FAIL");
    printf("Identifier : %u\n", ntohs(icmp->identifier));
    printf("Sequence   : %u\n", ntohs(icmp->sequence));
    printf("\n");
}
