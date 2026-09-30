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
     * 第一步：送出 SYN 完成三向交握
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

    printf("\n[Step 1] 發送 TCP SYN (SEQ=%u)...\n", client_seq);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    // 等待 SYN-ACK
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
                printf("[Step 1] 收到 SYN-ACK (Server SEQ=%u, ACK=%u)\n", server_seq, ntohl(rx_tcp->ack));
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

    printf("[Step 1] 發送交握 ACK (SEQ=%u, ACK=%u) -> 連線 ESTABLISHED！\n", client_seq + 1, server_seq + 1);
    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    /* ========================================================
     * 第二步：Client 發送資料觸發 Server 回傳 Payload
     * ======================================================== */
    const char *client_msg = "GET / HTTP/1.1\r\n\r\n";
    size_t msg_len = strlen(client_msg);
    unsigned char data_frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + msg_len];
    memset(data_frame, 0, sizeof(data_frame));

    struct ethernet_hdr *d_eth = (struct ethernet_hdr *)data_frame;
    struct ipv4_hdr *d_ip = (struct ipv4_hdr *)(data_frame + ETH_HEADER_LEN);
    struct tcp_hdr *d_tcp = (struct tcp_hdr *)(data_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *d_payload = data_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    memcpy(d_payload, client_msg, msg_len);
    memcpy(d_eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(d_eth->src, sender_mac, ETH_ADDR_LEN);
    d_eth->ethertype = htons(ETHERTYPE_IPV4);

    d_ip->version_ihl = 0x45;
    d_ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + msg_len);
    d_ip->identification = htons(1003);
    d_ip->ttl = 64;
    d_ip->protocol = IPPROTO_TCP;
    d_ip->src_ip = inet_addr("10.0.0.1");
    d_ip->dst_ip = inet_addr("10.0.0.2");
    d_ip->checksum = ipv4_checksum(d_ip, sizeof(struct ipv4_hdr));

    d_tcp->src_port = htons(client_port);
    d_tcp->dst_port = htons(server_port);
    d_tcp->seq = htonl(client_seq + 1); // 1001
    d_tcp->ack = htonl(server_seq + 1); // 5001
    d_tcp->data_offset = (5 << 4);
    d_tcp->flags = TCP_ACK | TCP_PSH;
    d_tcp->window = htons(4096);

    printf("\n[Step 2] 發送 Client 請求觸發 Server 回應...\n");
    sendto(sockfd, data_frame, sizeof(data_frame), 0, (struct sockaddr *)&sll, sizeof(sll));

    // 等待接收 Server 回傳的資料封包
    uint32_t s_data_seq = 0;
    size_t s_data_len = 0;
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP) continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            size_t rx_tcp_len = (rx_tcp->data_offset >> 4) * 4;
            size_t payload_len = ntohs(rx_ip->total_length) - rx_ip_len - rx_tcp_len;

            if (payload_len > 0) {
                s_data_seq = ntohl(rx_tcp->seq);
                s_data_len = payload_len;
                printf("[Step 2] ★ 成功收到 Server 資料！(SEQ=%u, Len=%zu)\n", s_data_seq, s_data_len);
                break;
            }
        }
    }

    /* ========================================================
     * 第三步：★ 測試 Fast Retransmit (送出 3 個 Duplicate ACK)
     * ======================================================== */
    printf("\n[Step 3] 故意送出 3 個 Duplicate ACK (ACK=%u) 測試快速重傳...\n", server_seq + 1);
    for (int i = 1; i <= 3; i++) {
        memset(frame, 0, sizeof(frame));
        memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
        memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
        eth->ethertype = htons(ETHERTYPE_IPV4);

        ip->version_ihl = 0x45;
        ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
        ip->identification = htons(1010 + i);
        ip->ttl = 64;
        ip->protocol = IPPROTO_TCP;
        ip->src_ip = inet_addr("10.0.0.1");
        ip->dst_ip = inet_addr("10.0.0.2");
        ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

        tcp->src_port = htons(client_port);
        tcp->dst_port = htons(server_port);
        tcp->seq = htonl(client_seq + 1 + msg_len);
        tcp->ack = htonl(server_seq + 1); // 依然要求 5001！
        tcp->data_offset = (5 << 4);
        tcp->flags = TCP_ACK;
        tcp->window = htons(4096);

        printf("  -> 發送 Duplicate ACK #%d (ACK=%u)\n", i, server_seq + 1);
        sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));
        usleep(50000); // 50ms
    }

    // 等待 Server 快速重傳的封包
    printf("[Step 3] 等待 Server Fast Retransmit 重傳封包...\n");
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP) continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            size_t rx_tcp_len = (rx_tcp->data_offset >> 4) * 4;
            size_t payload_len = ntohs(rx_ip->total_length) - rx_ip_len - rx_tcp_len;

            if (payload_len > 0 && ntohl(rx_tcp->seq) == s_data_seq) {
                printf("[Step 3] 成功收到 Server 快速重傳的封包 (SEQ=%u, Len=%zu)！\n",
                       ntohl(rx_tcp->seq), payload_len);
                break;
            }
        }
    }

    /* ========================================================
     * 第四步：★ 測試 Timeout Retransmission (靜默 1.5 秒不回 ACK)
     * ======================================================== */
    printf("\n[Step 4] 故意靜默不回覆 ACK，等待 1 秒 RTO 超時重傳...\n");
    while (1) {
        unsigned char rx_buf[2048];
        ssize_t n = recv(sockfd, rx_buf, sizeof(rx_buf), 0);
        if (n < (ssize_t)(ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)))
            continue;

        struct ipv4_hdr *rx_ip = (struct ipv4_hdr *)(rx_buf + ETH_HEADER_LEN);
        if (rx_ip->protocol != IPPROTO_TCP) continue;

        size_t rx_ip_len = (rx_ip->version_ihl & 0x0F) * 4;
        struct tcp_hdr *rx_tcp = (struct tcp_hdr *)(rx_buf + ETH_HEADER_LEN + rx_ip_len);

        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            size_t rx_tcp_len = (rx_tcp->data_offset >> 4) * 4;
            size_t payload_len = ntohs(rx_ip->total_length) - rx_ip_len - rx_tcp_len;

            if (payload_len > 0 && ntohl(rx_tcp->seq) == s_data_seq) {
                printf("[Step 4] 成功收到 Server 超時重傳的封包 (SEQ=%u, Len=%zu)！\n",
                       ntohl(rx_tcp->seq), payload_len);
                break;
            }
        }
    }

    /* ========================================================
     * 第五步：發送最終正確 ACK，釋放 Server 的 Send Buffer
     * ======================================================== */
    uint32_t final_ack = s_data_seq + s_data_len; // 5001 + 24 = 5025
    printf("\n[Step 5] 發送最終 ACK (ACK=%u) 確認收訖，測試 Server 釋放 Send Buffer...\n", final_ack);

    memset(frame, 0, sizeof(frame));
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ip->version_ihl = 0x45;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1020);
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2");
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    tcp->src_port = htons(client_port);
    tcp->dst_port = htons(server_port);
    tcp->seq = htonl(client_seq + 1 + msg_len);
    tcp->ack = htonl(final_ack);
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK;
    tcp->window = htons(4096);

    sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll));
    printf("全部測試完成！\n\n");

    close(sockfd);
    return 0;
}
