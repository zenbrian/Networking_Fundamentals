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

    printf("\n[1/3 Client] 發送 TCP SYN (SEQ=%u) 到 tap0...\n", client_seq);
    if (sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto SYN");
        close(sockfd);
        return 1;
    }

    /* ========================================================
     * 第二步：等待並接收 Server 的 SYN-ACK
     * ======================================================== */
    printf("[2/3 Client] 等待 Server 回傳 SYN-ACK...\n");
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

        // 檢查是否為我們的連線與 SYN-ACK
        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            if ((rx_tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                server_seq = ntohl(rx_tcp->seq);
                uint32_t acked_seq = ntohl(rx_tcp->ack);
                printf("[2/3 Client] 收到 Server 的 SYN-ACK！(Server SEQ=%u, ACK=%u)\n", server_seq, acked_seq);
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
    tcp->ack = htonl(server_seq + 1); // 5000 + 1 = 5001
    tcp->data_offset = (5 << 4);
    tcp->flags = TCP_ACK;
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    printf("[3/3 Client] 發送最終 ACK (SEQ=%u, ACK=%u) 完成三向交握！\n",
           client_seq + 1, server_seq + 1);

    if (sendto(sockfd, frame, sizeof(frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto ACK");
    } else {
        printf("\n三向交握封包全數發送完畢！請查看 ./network 終端機狀態！\n\n");
    }

        /* ========================================================
     * 第四步：連線已 ESTABLISHED，Client 發送 HTTP GET 請求！
     * ======================================================== */
    const char *http_req = "GET / HTTP/1.1\r\nHost: 10.0.0.2\r\n\r\n";
    size_t req_len = strlen(http_req);

    // Buffer 大小 = L2 + L3 + L4 + Payload 長度
    unsigned char data_frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + req_len];
    memset(data_frame, 0, sizeof(data_frame));

    struct ethernet_hdr *d_eth = (struct ethernet_hdr *)data_frame;
    struct ipv4_hdr *d_ip = (struct ipv4_hdr *)(data_frame + ETH_HEADER_LEN);
    struct tcp_hdr *d_tcp = (struct tcp_hdr *)(data_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *d_payload = data_frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    // 複製 HTTP Payload
    memcpy(d_payload, http_req, req_len);

    // Ethernet
    memcpy(d_eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(d_eth->src, sender_mac, ETH_ADDR_LEN);
    d_eth->ethertype = htons(ETHERTYPE_IPV4);

    // IPv4 (注意 total_length 要加上 req_len！)
    d_ip->version_ihl = 0x45;
    d_ip->tos = 0;
    d_ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + req_len);
    d_ip->identification = htons(1003);
    d_ip->flags_fragment = 0;
    d_ip->ttl = 64;
    d_ip->protocol = IPPROTO_TCP;
    d_ip->src_ip = inet_addr("10.0.0.1");
    d_ip->dst_ip = inet_addr("10.0.0.2");
    d_ip->checksum = 0;
    d_ip->checksum = ipv4_checksum(d_ip, sizeof(struct ipv4_hdr));

    // TCP (Flags 可以是 TCP_ACK 或 TCP_ACK | TCP_PSH)
    d_tcp->src_port = htons(client_port);
    d_tcp->dst_port = htons(server_port);
    d_tcp->seq = htonl(client_seq + 1); // 1001
    d_tcp->ack = htonl(server_seq + 1); // 5001
    d_tcp->data_offset = (5 << 4);
    d_tcp->flags = TCP_ACK | TCP_PSH;
    d_tcp->window = htons(4096);
    d_tcp->checksum = 0;
    d_tcp->urgent_ptr = 0;

    printf("[4/4 Client] 發送 HTTP Request (%zu bytes) 到 tap0...\n", req_len);
    if (sendto(sockfd, data_frame, sizeof(data_frame), 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto Data");
    }
        /* ========================================================
     * 第五步：等待 Server 回傳確認資料收到的純 ACK
     * ======================================================== */
    printf("[5/4 Client] 等待 Server 回傳資料的 ACK...\n");
    uint32_t expected_data_ack = (client_seq + 1) + req_len; // 1001 + 34 = 1035

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

        // 檢查是否是 Server 回給我們的 ACK
        if (ntohs(rx_tcp->dst_port) == client_port && ntohs(rx_tcp->src_port) == server_port) {
            if (rx_tcp->flags & TCP_ACK) {
                uint32_t acked = ntohl(rx_tcp->ack);
                printf("\n========================================\n");
                printf("[Client] 成功收到 Server 回傳的 ACK！(ACK=%u)\n", acked);
                if (acked == expected_data_ack) {
                    printf("[Client] 驗證成功！Server 正確確認了全部 %zu bytes 資料 (1001 + %zu = %u)！\n",
                           req_len, req_len, expected_data_ack);
                } else {
                    printf("[Client] 警告：ACK 號碼不符 (收到 %u, 期望 %u)\n", acked, expected_data_ack);
                }
                printf("========================================\n\n");
                break;
            }
        }
    }

    /* ========================================================
     * 第六步：等待接收 Server 主動回傳的 Payload！
     * ======================================================== */
    printf("[6/4 Client] 等待 Server 回傳應用層資料 (Data Segment)...\n");
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
            size_t rx_tcp_len = (rx_tcp->data_offset >> 4) * 4;
            size_t rx_payload_len = ntohs(rx_ip->total_length) - rx_ip_len - rx_tcp_len;

            // 只要有收到資料 (Payload > 0)
            if (rx_payload_len > 0) {
                uint8_t *rx_payload = rx_buf + ETH_HEADER_LEN + rx_ip_len + rx_tcp_len;
                printf("\n[Client 成功收到 Server 回覆！]\n");
                printf("長度：%zu Bytes | SEQ=%u, ACK=%u\n", rx_payload_len, ntohl(rx_tcp->seq), ntohl(rx_tcp->ack));
                printf("內容：");
                fwrite(rx_payload, 1, rx_payload_len, stdout);
                printf("\n================================================\n\n");
                break;
            }
        }
    }

    close(sockfd);
    return 0;
}
