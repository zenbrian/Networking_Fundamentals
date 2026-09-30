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

    printf("\n=== [Step 1: 三向交握建立 ESTABLISHED] ===\n");
    printf("[Handshake] Client 發送 SYN (SEQ=%u)...\n", client_seq);
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

    printf("[Handshake] 發送交握 ACK (SEQ=%u, ACK=%u) -> 連線 ESTABLISHED！\n", 
           client_seq + 1, server_seq + 1);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    /* ========================================================
     * 第二階段：Client 發送請求，觸發 Server 主動關閉 (tcp_close)
     * ======================================================== */
    const char *req = "GET / HTTP/1.1\r\nConnection: CLOSE\r\n\r\n";
    size_t req_len = strlen(req);

    unsigned char req_frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + req_len];
    memset(req_frame, 0, sizeof(req_frame));

    struct ethernet_hdr *r_eth = (struct ethernet_hdr *)req_frame;
    struct ipv4_hdr *r_ip = (struct ipv4_hdr *)(req_frame + ETH_HEADER_LEN);
    struct tcp_hdr *r_tcp = (struct tcp_hdr *)(req_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *r_payload = req_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    memcpy(r_payload, req, req_len);
    memcpy(r_eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(r_eth->src, sender_mac, ETH_ADDR_LEN);
    r_eth->ethertype = htons(ETHERTYPE_IPV4);

    r_ip->version_ihl = 0x45;
    r_ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + req_len);
    r_ip->identification = htons(1003);
    r_ip->ttl = 64;
    r_ip->protocol = IPPROTO_TCP;
    r_ip->src_ip = inet_addr("10.0.0.1");
    r_ip->dst_ip = inet_addr("10.0.0.2");
    r_ip->checksum = ipv4_checksum(r_ip, sizeof(struct ipv4_hdr));

    r_tcp->src_port = htons(client_port);
    r_tcp->dst_port = htons(server_port);
    r_tcp->seq = htonl(client_seq + 1); // 1001
    r_tcp->ack = htonl(server_seq + 1); // 5001
    r_tcp->data_offset = (5 << 4);
    r_tcp->flags = TCP_ACK | TCP_PSH;
    r_tcp->window = htons(4096);

    printf("\n=== [Step 2: Client 發送含 CLOSE 的請求，觸發 Server 主動關閉] ===\n");
    printf("[Request] 發送帶有 'Connection: CLOSE' 的請求...\n");
    sendto(sockfd, req_frame, sizeof(req_frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    /* ========================================================
     * 第三階段：接收 Server 回傳的資料與 Server 主動送出的 FIN
     * (第 1 揮：Server 主動呼叫 tcp_close() 送出 FIN-ACK，Server 進入 FIN_WAIT_1)
     * ======================================================== */
    printf("\n=== [Step 3: 等待 Server 回傳資料與 Server 主動送出的第 1 揮 (FIN)] ===\n");
    uint32_t server_fin_seq = 0;
    int got_data = 0;
    int got_server_fin = 0;

    while (!got_data || !got_server_fin) {
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
            size_t rx_tcp_len = (rx_tcp->data_offset >> 4) * 4;
            size_t payload_len = ntohs(rx_ip->total_length) - rx_ip_len - rx_tcp_len;

            // 收到 Server 的 Response Payload
            if (payload_len > 0) {
                got_data = 1;
                printf("[Response] 收到 Server 回傳資料：%zu bytes\n", payload_len);
            }

            // 收到 Server 主動送出的 FIN
            if (rx_tcp->flags & TCP_FIN) {
                got_server_fin = 1;
                server_fin_seq = ntohl(rx_tcp->seq);
                printf("[Wave 1/4] ★ 收到 Server 主動送出的 FIN！(Server SEQ=%u) -> Server 進入 FIN_WAIT_1\n", 
                       server_fin_seq);
            }
        }
    }

    sleep(1);

    /* ========================================================
     * 第四階段：完成四向揮手對接
     * 第 2 揮：Client 回覆 ACK (確認 Server 的 FIN) -> Server 進入 FIN_WAIT_2
     * ======================================================== */
    uint32_t client_next_seq = client_seq + 1 + req_len; // 1001 + 41 = 1042
    uint32_t ack_for_server_fin = server_fin_seq + 1;    // Server FIN 消耗 1 號

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
    tcp->seq = htonl(client_next_seq);
    tcp->ack = htonl(ack_for_server_fin);
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK;
    tcp->window = htons(4096);

    printf("\n=== [Step 4: Client 回覆 ACK，推動 Server 進入 FIN_WAIT_2] ===\n");
    printf("[Wave 2/4] Client 送出 ACK (ACK=%u) -> Server 進入 FIN_WAIT_2！\n", ack_for_server_fin);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    sleep(1);

    /* ========================================================
     * 第 3 揮：Client 也送出自己的 FIN-ACK -> 觸發 Server 進入 TIME_WAIT！
     * ======================================================== */
    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1005);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_next_seq);
    tcp->ack = htonl(ack_for_server_fin);
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_FIN | TCP_ACK; // ★ Client 的 FIN
    tcp->window = htons(4096);

    printf("\n=== [Step 5: Client 送出 FIN，推動 Server 進入 TIME_WAIT] ===\n");
    printf("[Wave 3/4] Client 送出 FIN-ACK (SEQ=%u) -> 等待 Server 回覆最後 ACK 並進入 TIME_WAIT...\n",
           client_next_seq);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    // 等待接收 Server 回覆的第 4 揮最終 ACK
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
            if ((rx_tcp->flags & TCP_ACK) && !(rx_tcp->flags & TCP_FIN)) {
                printf("[Wave 4/4] ★ 成功收到 Server 最終 ACK (ACK=%u)！\n", ntohl(rx_tcp->ack));
                printf("           此時 Server 已正式進入 TIME_WAIT 狀態，啟動 2 秒倒數！\n");
                break;
            }
        }
    }

    /* ========================================================
     * 第五階段：靜待 3 秒，觀察 Server 端的 TIME_WAIT 到期釋放
     * ======================================================== */
    printf("\n=== [Step 6: 觀察 TIME_WAIT 到期釋放] ===\n");
    printf("[Waiting] Client 停留 3 秒... 請觀察 Server 終端機在 2 秒後是否自動轉為 CLOSED！\n");
    for (int s = 1; s <= 3; s++) {
        sleep(1);
        printf("  ...等待中 (%d 秒)\n", s);
    }

    printf("\n🎉 Server 主動關閉與 TIME_WAIT 驗證流程全部結束！\n\n");

    close(sockfd);
    return 0;
}
