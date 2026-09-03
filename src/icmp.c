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

void icmp_handle(uint8_t *packet, size_t length)
{
    if (length < sizeof(struct icmp_hdr))
        return;

    struct icmp_hdr *icmp = (struct icmp_hdr *)packet;

    // 只處理 Echo Request (Type = 8)
    if (icmp->type != ICMP_ECHO_REQUEST) {
        return;
    }

    printf("[ICMP] Echo Request received -> Generating Echo Reply\n");

    // 1. 修改 Type 為 Echo Reply (0)，Code 保持 0
    icmp->type = ICMP_ECHO_REPLY;

    // 2. 重新計算 ICMP Checksum (包含 Header + 所有 Payload)
    icmp->checksum = 0;
    icmp->checksum = ipv4_checksum(packet, length);
}
