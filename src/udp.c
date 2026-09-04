#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ethernet.h"
#include "ipv4.h"
#include "udp.h"

/* Socket Table: 紀錄本機正在監聽哪些 Port */
static struct udp_socket udp_table[MAX_UDP_SOCKETS];

void udp_init(void)
{
    memset(udp_table, 0, sizeof(udp_table));
}

int udp_bind(uint16_t port)
{
    if (port == 0) {
        return -1;
    }

    // 檢查是否已被綁定
    if (udp_lookup(port) != NULL) {
        return -1;
    }

    // 找一個空位置存放
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_table[i].port == 0) {
            udp_table[i].port = port;
            printf("[UDP] Bound to port %u\n", port);
            return 0;
        }
    }

    return -1; // 表格已滿
}

struct udp_socket *udp_lookup(uint16_t port)
{
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_table[i].port == port) {
            return &udp_table[i];
        }
    }
    return NULL;
}

void udp_print_header(const struct udp_hdr *udp)
{
    printf("\n");
    printf("UDP Packet\n");
    printf("-------------------\n");
    printf("Source Port      : %u\n", ntohs(udp->src_port));
    printf("Destination Port : %u\n", ntohs(udp->dst_port));
    printf("Length           : %u\n", ntohs(udp->length));
    printf("Checksum         : 0x%04x\n", ntohs(udp->checksum));
    printf("\n");
}

void udp_receive(int fd, const uint8_t *frame, size_t len)
{
    (void)fd; // 目前只讀取，明天 Day 13 會用到 fd 來回傳

    // 基本長度校驗：Ethernet + IPv4 + UDP
    if (len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr)) {
        return;
    }

    const uint8_t *payload = frame + ETH_HEADER_LEN;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ihl + sizeof(struct udp_hdr)) {
        return;
    }

    const struct udp_hdr *udp = (const struct udp_hdr *)(payload + ihl);

    // 1. 印出 UDP Header
    udp_print_header(udp);

    // 2. 取得目的 Port (需從 Network Byte Order 轉回 Host Byte Order)
    uint16_t dst_port = ntohs(udp->dst_port);

    // 3. Port Lookup: 查詢 Socket Table
    struct udp_socket *sock = udp_lookup(dst_port);

    if (sock) {
        printf("[UDP] Deliver to Port %u\n", dst_port);
    } else {
        printf("[UDP] No listener\n");
    }
    fflush(stdout);
}
