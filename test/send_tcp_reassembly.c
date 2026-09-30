#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/if_ether.h>

#include "config.h"
#include "ethernet.h"
#include "ipv4.h"
#include "tcp.h"
#include "checksum.h"

int main(void)
{
    // 1. 建立 AF_PACKET Raw Socket
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket (請使用 sudo 或確認權限)");
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex("tap0");
    sll.sll_protocol = htons(ETH_P_ALL);

    if (sll.sll_ifindex == 0) {
        fprintf(stderr, "找不到 tap0 介面！請先確認在另一個終端機執行 ./network\n");
        close(sockfd);
        return 1;
    }

    uint8_t sender_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint16_t client_port = 52144;
    uint16_t server_port = 8080;
    uint32_t client_seq = 1000;

    /* ========================================================
     * 第一步：送出 SYN
     * ======================================================== */
    unsigned char frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(frame, 0, sizeof(frame));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // Ethernet Header
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // IPv4 Header
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1001);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // TCP Header (SYN)
    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_seq);
    tcp->ack = 0;
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_SYN;
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    printf("\n[1/4 Client] 發送 TCP SYN (SEQ=%u) 到 tap0...\n", client_seq);
    if (sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto SYN");
        close(sockfd);
        return 1;
    }

    /* ========================================================
     * 第二步：等待並接收 Server 的 SYN-ACK
     * ======================================================== */
    printf("[2/4 Client] 等待 Server 回傳 SYN-ACK...\n");
    uint32_t server_seq = 0;
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)rx_buf;
        if (ntohs(rx_eth->ethertype) != ETHERTYPE_IPV4)
            continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP)
            continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            if ((rx_tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                server_seq = ntohl(rx_tcp->seq);
                uint32_t acked_seq = ntohl(rx_tcp->ack);
                printf("[2/4 Client] 收到 Server 的 SYN-ACK！(Server SEQ=%u, ACK=%u)\n", server_seq, acked_seq);
                break;
            }
        }
    }

    /* ========================================================
     * 第三步：送出 ACK，完成三向交握！
     * ======================================================== */
    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1002);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_seq + 1); // 1001
    tcp->ack = htonl(server_seq + 1); // 5001
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK;
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    printf("[3/4 Client] 發送 ACK (SEQ=%u, ACK=%u) 完成三向交握！\n", client_seq + 1, server_seq + 1);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    /* ========================================================
     * 第四步：★ 故意製造亂序 (Out-of-Order Delivery) ★
     * 欲傳送完整內容："Hello World\n"
     * Packet 1: SEQ=1001, Data="Hello " (Len=6)
     * Packet 2: SEQ=1007, Data="World\n" (Len=6)
     *
     * 刻意發送順序：先發送 Packet 2 (1007)，再發送 Packet 1 (1001)
     * ======================================================== */
    const char *p1_data = "Hello ";
    size_t p1_len = strlen(p1_data); // 6
    uint32_t p1_seq = 1001;

    const char *p2_data = "World\n";
    size_t p2_len = strlen(p2_data); // 6
    uint32_t p2_seq = 1001 + p1_len; // 1007

    // --- (A) 先發送 Packet 2 (SEQ=1007, "World\n") ---
    unsigned char frame_p2[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + p2_len];
    memset(frame_p2, 0, sizeof(frame_p2));
    struct ethernet_hdr *eth2 = (struct ethernet_hdr *)frame_p2;
    struct ipv4_hdr *ip2 = (struct ipv4_hdr *)(frame_p2 + ETH_HEADER_LEN);
    struct tcp_hdr *tcp2 = (struct tcp_hdr *)(frame_p2 + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *payload2 = frame_p2 + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    memcpy(payload2, p2_data, p2_len);
    memcpy(eth2->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth2->src, sender_mac, ETH_ADDR_LEN);
    eth2->ethertype = htons(ETHERTYPE_IPV4);

    ip2->version_ihl = 0x45;
    ip2->tos = 0;
    ip2->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + p2_len);
    ip2->identification = htons(1004);
    ip2->flags_fragment = 0;
    ip2->ttl = 64;
    ip2->protocol = IPPROTO_TCP;
    ip2->src_ip = inet_addr("10.0.0.1");
    ip2->dst_ip = inet_addr("10.0.0.2");
    ip2->checksum = 0;
    ip2->checksum = ipv4_checksum(ip2, sizeof(struct ipv4_hdr));

    tcp2->src_port = htons(client_port);
    tcp2->dst_port = htons(server_port);
    tcp2->seq = htonl(p2_seq); // 1007
    tcp2->ack = htonl(server_seq + 1);
    tcp2->data_offset = (5 << 4);
    tcp2->flags = TCP_ACK | TCP_PSH;
    tcp2->window = htons(4096);

    printf("\n[4/4 Client] ★ 刻意【先】發送 Packet 2 (SEQ=%u, Payload=\"%s\")\n", p2_seq, "World\\n");
    sendto(sockfd, frame_p2, sizeof(frame_p2), 0, (struct sockaddr *)&sll, sizeof(sll));

    sleep(1); // 停頓 1 秒讓 LOG 顯示更清晰

    // --- (B) 後發送 Packet 1 (SEQ=1001, "Hello ") ---
    unsigned char frame_p1[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + p1_len];
    memset(frame_p1, 0, sizeof(frame_p1));
    struct ethernet_hdr *eth1 = (struct ethernet_hdr *)frame_p1;
    struct ipv4_hdr *ip1 = (struct ipv4_hdr *)(frame_p1 + ETH_HEADER_LEN);
    struct tcp_hdr *tcp1 = (struct tcp_hdr *)(frame_p1 + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *payload1 = frame_p1 + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    memcpy(payload1, p1_data, p1_len);
    memcpy(eth1->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth1->src, sender_mac, ETH_ADDR_LEN);
    eth1->ethertype = htons(ETHERTYPE_IPV4);

    ip1->version_ihl = 0x45;
    ip1->tos = 0;
    ip1->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + p1_len);
    ip1->identification = htons(1005);
    ip1->flags_fragment = 0;
    ip1->ttl = 64;
    ip1->protocol = IPPROTO_TCP;
    ip1->src_ip = inet_addr("10.0.0.1");
    ip1->dst_ip = inet_addr("10.0.0.2");
    ip1->checksum = 0;
    ip1->checksum = ipv4_checksum(ip1, sizeof(struct ipv4_hdr));

    tcp1->src_port = htons(client_port);
    tcp1->dst_port = htons(server_port);
    tcp1->seq = htonl(p1_seq); // 1001
    tcp1->ack = htonl(server_seq + 1);
    tcp1->data_offset = (5 << 4);
    tcp1->flags = TCP_ACK | TCP_PSH;
    tcp1->window = htons(4096);

    printf("[4/4 Client] ★ 刻意【後】發送 Packet 1 (SEQ=%u, Payload=\"%s\")\n", p1_seq, p1_data);
    sendto(sockfd, frame_p1, sizeof(frame_p1), 0, (struct sockaddr *)&sll, sizeof(sll));

    /* ========================================================
     * 第五步：收 ACK 觀察 Server 的 Cumulative ACK
     * ======================================================== */
    printf("\n[5/4 Client] 等待 Server 回傳 Cumulative ACK...\n");
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)rx_buf;
        if (ntohs(rx_eth->ethertype) != ETHERTYPE_IPV4)
            continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP)
            continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            if (rx_tcp->flags & TCP_ACK) {
                uint32_t acked = ntohl(rx_tcp->ack);
                printf("[Client 收到 ACK] ACK=%u\n", acked);
                if (acked == 1001 + p1_len + p2_len) {
                    printf("\n🎉 驗收成功！Server 的 Cumulative ACK 已推進到 %u（完整接收並重組 Hello World）！\n\n", acked);
                    break;
                }
            }
        }
    }

    close(sockfd);
    return 0;
}
