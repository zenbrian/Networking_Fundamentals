#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>     // 提供 write()

#include "ethernet.h"
#include "ipv4.h"
#include "udp.h"
#include "checksum.h"   // 提供 ipv4_checksum()
#include "arp_table.h"  // 提供 arp_table_lookup()
#include "config.h"     // 提供 LOCAL_MAC 與 LOCAL_IP


/* Socket Table: 紀錄本機正在監聽哪些 Port */
static struct udp_socket udp_table[MAX_UDP_SOCKETS];

void udp_init(void)
{
    memset(udp_table, 0, sizeof(udp_table));
}

int udp_bind(uint16_t port, udp_handler_t handler)
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
            udp_table[i].handler = handler;
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

void udp_dump_payload(const uint8_t *payload, size_t len)
{
    printf("Payload (%zu bytes):\n", len);
    for (size_t i = 0; i < len; i++) {
        putchar(payload[i]);
    }
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

    uint16_t udp_len = ntohs(udp->length);
    // 防禦性檢查：封包標示長度不能小於 UDP Header 本身 (8 bytes)
    if (udp_len < sizeof(struct udp_hdr)) {
        return;
    }
    size_t payload_len = udp_len - sizeof(struct udp_hdr);
    const uint8_t *udp_payload = (const uint8_t *)udp + sizeof(struct udp_hdr);


    if (sock) {
        printf("[UDP] Deliver to Port %u\n", dst_port);
        // 印出 Payload 內容
        udp_dump_payload(udp_payload, payload_len);
        // 如果該 Socket 有註冊 Handler，就呼叫它（執行應用程式）！
        if (sock->handler) {
            sock->handler(udp_payload, payload_len);
        }
    } else {
        printf("[UDP] No listener\n");
    }
    fflush(stdout);
}


int udp_send(int fd, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *data, size_t len)
{
    // 1. 查詢 ARP 表：要送到目標 IP，必須先知道對方的 MAC 位址
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&dst_ip);
    if (!entry || !entry->valid) {
        printf("[UDP] Send failed: Destination MAC not in ARP table\n");
        return -1;
    }

    // 2. 準備緩衝區，並切分各層標頭的指標位置
    uint8_t buffer[1514];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct udp_hdr *udp = (struct udp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *payload = buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr);

    // 3. 填充 Payload (應用層資料)
    memcpy(payload, data, len);

    // 4. 封裝 UDP Header (L4)
    udp->src_port = htons(src_port);                   // 動態來源 Port (可為 8080 或臨時 Port)
    udp->dst_port = htons(dst_port);                   // 目標 Port
    udp->length = htons(sizeof(struct udp_hdr) + len); // UDP 標頭長度(8) + 資料長度
    udp->checksum = 0;                                 // IPv4 下 UDP Checksum 設為 0 代表不校驗

    // 5. 封裝 IPv4 Header (L3)
    ip->version_ihl = 0x45;                            // IPv4, 標頭長度 20 bytes (IHL=5)
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + len);
    ip->identification = htons(1);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = 17;                                 // 17 代表 UDP
    ip->src_ip = *(uint32_t *)LOCAL_IP;                // 10.0.0.2
    ip->dst_ip = dst_ip;                               // 目標 IP
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr)); // 計算 IP 標頭校驗和

    // 6. 封裝 Ethernet Header (L2)
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);        // 從 ARP 快取取得目標 MAC
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);         // 本機 MAC
    eth->ethertype = htons(ETHERTYPE_IPV4);            // 0x0800 代表 IPv4

    // 7. 透過 TAP 虛擬網卡送出 Frame
    size_t total_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr) + len;
    ssize_t sent = write(fd, buffer, total_len);
    if (sent < 0) {
        perror("[UDP] write failed");
        return -1;
    }

    printf("[UDP] Successfully sent %ld bytes from port %u to port %u\n", sent, src_port, dst_port);
    fflush(stdout);
    return 0;
}
