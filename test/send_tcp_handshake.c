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
        printf("\n🎉 三向交握封包全數發送完畢！請查看 ./network 終端機狀態！\n\n");
    }

    close(sockfd);
    return 0;
}
