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
        perror("socket");
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex("tap0");
    sll.sll_protocol = htons(ETH_P_ALL);

    if (sll.sll_ifindex == 0) {
        fprintf(stderr, "找不到 tap0 介面！請先在另一個終端機執行 ./network\n");
        close(sockfd);
        return 1;
    }

    uint8_t sender_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint16_t client_port = 52144;
    uint16_t server_port = 8080;
    uint32_t client_seq = 1000;

    /* ========================================================
     * 第一階段：完成 TCP 三向交握 (建立 ESTABLISHED 連線)
     * ======================================================== */
    unsigned char frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(frame, 0, sizeof(frame));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1001);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_seq);
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_SYN;
    tcp->window = htons(4096);

    printf("\n=== [Phase 1: 建立連線 (Handshake)] ===\n");
    printf("[Handshake] 發送 TCP SYN (SEQ=%u)...\n", client_seq);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    // 等待接收 SYN-ACK
    uint32_t server_seq = 0;
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)rx_buf;
        if (ntohs(rx_eth->ethertype) != ETHERTYPE_IPV4) continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP) continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            if ((rx_tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                server_seq = ntohl(rx_tcp->seq);
                printf("[Handshake] 收到 Server SYN-ACK (Server SEQ=%u, ACK=%u)\n", 
                       server_seq, ntohl(rx_tcp->ack));
                break;
            }
        }
    }

    // 發送第三次交握 ACK
    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1002);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_seq + 1); // 1001
    tcp->ack = htonl(server_seq + 1); // 5001
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK;
    tcp->window = htons(4096);

    printf("[Handshake] 發送交握 ACK (SEQ=%u, ACK=%u) -> 連線 ESTABLISHED！\n\n", 
           client_seq + 1, server_seq + 1);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    sleep(1); // 稍停 1 秒讓交握日誌更清晰

    /* ========================================================
     * 第二階段：TCP 四向揮手 (Four-Way Teardown)
     * ======================================================== */
    printf("=== [Phase 2: TCP Four-Way Teardown (連線關閉)] ===\n");

    // --------------------------------------------------------
    // 第 1 次揮手：Client 主動發送 FIN-ACK
    // --------------------------------------------------------
    uint32_t client_fin_seq = client_seq + 1; // 1001
    uint32_t current_server_ack = server_seq + 1; // 5001

    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1003);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_fin_seq); // 1001
    tcp->ack = htonl(current_server_ack); // 5001
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_FIN | TCP_ACK; // ★ FIN + ACK
    tcp->window = htons(4096);

    printf("[Wave 1/4] Client 發送 FIN-ACK (SEQ=%u, ACK=%u) -> 狀態: FIN_WAIT_1\n",
           client_fin_seq, current_server_ack);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    // --------------------------------------------------------
    // 等待接收 Server 的回應：
    // 第 2 次揮手：Server 回覆的 ACK (確認 Client 的 FIN，ACK=1002)
    // 第 3 次揮手：Server 送出的 FIN-ACK (Server 也宣告關閉)
    // --------------------------------------------------------
    int got_ack = 0;
    int got_fin = 0;
    uint32_t server_fin_seq = 0;

    printf("[Waiting] 等待 Server 回應第 2 揮 (ACK) 與第 3 揮 (FIN)...\n");

    while (!got_ack || !got_fin) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)rx_buf;
        if (ntohs(rx_eth->ethertype) != ETHERTYPE_IPV4) continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP) continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            uint32_t rx_ack = ntohl(rx_tcp->ack);
            uint32_t rx_seq = ntohl(rx_tcp->seq);

            // 檢查第 2 次揮手：純 ACK (確認收到 Client 的 FIN，ACK 應為 1002)
            if (!got_ack && (rx_tcp->flags & TCP_ACK) && !(rx_tcp->flags & TCP_FIN)) {
                if (rx_ack == client_fin_seq + 1) {
                    got_ack = 1;
                    printf("[Wave 2/4] ★ 收到 Server 回覆 ACK (ACK=%u) -> Client 進入 FIN_WAIT_2\n", rx_ack);
                }
            }

            // 檢查第 3 次揮手：Server 發送的 FIN
            if (!got_fin && (rx_tcp->flags & TCP_FIN)) {
                got_fin = 1;
                server_fin_seq = rx_seq;
                printf("[Wave 3/4] ★ 收到 Server 發送 FIN-ACK (Server SEQ=%u) -> Server 進入 LAST_ACK\n", 
                       server_fin_seq);
            }
        }
    }

    sleep(1); // 停頓 1 秒展示流程

    // --------------------------------------------------------
    // 第 4 次揮手：Client 發送最終 ACK 確認 Server 的 FIN
    // --------------------------------------------------------
    uint32_t final_client_seq = client_fin_seq + 1; // 1002
    uint32_t final_ack = server_fin_seq + 1;        // 5001 + 1 = 5002 (Server 的 FIN 也消耗 1 號)

    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1004);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(final_client_seq);
    tcp->ack = htonl(final_ack); // 5002
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK; // 純 ACK
    tcp->window = htons(4096);

    printf("[Wave 4/4] Client 發送最終 ACK (SEQ=%u, ACK=%u) -> 連線正式 CLOSED！\n",
           final_client_seq, final_ack);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    printf("\n🎉 TCP 四向揮手 (Four-Way Teardown) 測試圓滿成功！\n\n");

    close(sockfd);
    return 0;
}
