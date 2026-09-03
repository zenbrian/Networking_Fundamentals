#include <stdio.h>
#include <string.h>    // 需要 memcpy, memset
#include <unistd.h>    // 需要 write
#include <arpa/inet.h>

#include "ethernet.h"  // <-- 補上這行
#include "ipv4.h"      // <-- 補上這行
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


void icmp_send_time_exceeded(int fd, const uint8_t *orig_frame, size_t orig_len)
{
    // 檢查原封包長度是否至少有 Ethernet + IPv4 標頭
    if (orig_len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr))
        return;

    const struct ethernet_hdr *orig_eth = (const struct ethernet_hdr *)orig_frame;
    const struct ipv4_hdr *orig_ip = (const struct ipv4_hdr *)(orig_frame + ETH_HEADER_LEN);

    // 取得原始 IPv4 Header 的長度 (IHL * 4)
    uint8_t orig_ihl = (orig_ip->version_ihl & 0x0F) * 4;

    // 計算要保留的原封包內容：原 IP Header + 原 Payload 的前 8 bytes
    size_t orig_ip_payload_len = orig_len - ETH_HEADER_LEN - orig_ihl;
    size_t copy_payload_len = (orig_ip_payload_len >= 8) ? 8 : orig_ip_payload_len;
    size_t icmp_data_len = orig_ihl + copy_payload_len;

    // 計算封包總長度
    size_t icmp_total_len = sizeof(struct icmp_hdr) + icmp_data_len;
    size_t reply_frame_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + icmp_total_len;

    uint8_t reply[2048];
    memset(reply, 0, reply_frame_len);

    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
    struct icmp_hdr *icmp_out = (struct icmp_hdr *)(reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *icmp_payload = reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr);

    // 1. Ethernet Header (對調 MAC)
    memcpy(eth_out->dst, orig_eth->src, ETH_ADDR_LEN);
    memcpy(eth_out->src, orig_eth->dst, ETH_ADDR_LEN);
    eth_out->ethertype = htons(ETHERTYPE_IPV4);

    // 2. IPv4 Header
    ip_out->version_ihl = 0x45;
    ip_out->tos = 0;
    ip_out->total_length = htons(sizeof(struct ipv4_hdr) + icmp_total_len);
    ip_out->identification = htons(0);
    ip_out->flags_fragment = 0;
    ip_out->ttl = 64;
    ip_out->protocol = IPPROTO_ICMP;
    ip_out->src_ip = orig_ip->dst_ip; // 誰逾期就由誰回覆 (10.0.0.2)
    ip_out->dst_ip = orig_ip->src_ip; // 回給來源端 (10.0.0.1)
    ip_out->checksum = 0;
    ip_out->checksum = ipv4_checksum(ip_out, sizeof(struct ipv4_hdr));

    // 3. ICMP Header (Type 11, Code 0, Unused 填 0)
    icmp_out->type = ICMP_TIME_EXCEEDED;
    icmp_out->code = 0;
    icmp_out->checksum = 0;
    icmp_out->identifier = 0;
    icmp_out->sequence = 0;

    // 4. 複製原 IP 標頭 + 前 8 bytes 資料到 ICMP Payload
    memcpy(icmp_payload, orig_ip, icmp_data_len);

    // 5. 計算 ICMP Checksum
    icmp_out->checksum = ipv4_checksum(icmp_out, icmp_total_len);

    // 6. 送出
    ssize_t sent = write(fd, reply, reply_frame_len);
    if (sent < 0) {
        perror("[ICMP] write Time Exceeded failed");
    } else {
        printf("[ICMP] Time Exceeded (Type 11, Code 0) sent to ");
        ipv4_print_ip(ip_out->dst_ip);
        printf("\n");
        fflush(stdout);
    }
}

void icmp_receive(int fd, const uint8_t *frame, size_t len)
{
    // 檢查封包長度是否足夠容納 Ethernet + IPv4 + ICMP
    if (len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr))
        return;

    const uint8_t *payload = frame + ETH_HEADER_LEN;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ihl + sizeof(struct icmp_hdr))
        return;

    const struct icmp_hdr *icmp_in = (const struct icmp_hdr *)(payload + ihl);
    size_t icmp_in_len = len - ETH_HEADER_LEN - ihl;

    // 印出接收到的 ICMP 標頭
    icmp_print_header(icmp_in, icmp_in_len);

    // 如果不是 Echo Request 就忽略
    if (icmp_in->type != ICMP_ECHO_REQUEST) {
        return;
    }

    // 複製原封包以製作 Reply
    uint8_t reply[2048];
    memcpy(reply, frame, len);

    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
    uint8_t *icmp_out = reply + ETH_HEADER_LEN + ihl;
    size_t icmp_len = len - ETH_HEADER_LEN - ihl;

    // 1. 交換 Ethernet MAC
    uint8_t temp_mac[ETH_ADDR_LEN];
    memcpy(temp_mac, eth_out->dst, ETH_ADDR_LEN);
    memcpy(eth_out->dst, eth_out->src, ETH_ADDR_LEN);
    memcpy(eth_out->src, temp_mac, ETH_ADDR_LEN);

    // 2. 交換 IPv4 來源與目的 IP，並更新 TTL 與 Checksum
    uint32_t temp_ip = ip_out->dst_ip;
    ip_out->dst_ip = ip_out->src_ip;
    ip_out->src_ip = temp_ip;
    ip_out->ttl = 64;
    ip_out->checksum = 0;
    ip_out->checksum = ipv4_checksum(ip_out, ihl);

    // 3. 修改 ICMP 為 Echo Reply 並重算 Checksum
    icmp_handle(icmp_out, icmp_len);

    // 4. 送出 Reply Frame 回 TAP 網卡
    ssize_t sent = write(fd, reply, len);
    if (sent < 0) {
        perror("[ICMP] write Echo Reply failed");
    } else {
        printf("[ICMP] Echo Reply sent (%ld bytes)\n", sent);
        fflush(stdout);
    }
}
