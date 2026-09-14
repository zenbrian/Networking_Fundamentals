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
        fprintf(stderr, "找不到 tap0 介面！請先在另一個終端機執行 ./network\n");
        close(sockfd);
        return 1;
    }

    // 2. 準備 Buffer 空間：Ethernet(14) + IPv4(20) + TCP(20) = 54 Bytes
    unsigned char frame[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(frame, 0, sizeof(frame));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(frame + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 3. 填充 Layer 2: Ethernet Header
    uint8_t sender_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN); // 送給我的 TAP 網卡
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 4. 填充 Layer 3: IPv4 Header
    ip->version_ihl = 0x45; // Version 4, IHL 5 (20 bytes)
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(1001);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP; // 6: TCP
    ip->src_ip = inet_addr("10.0.0.1");
    ip->dst_ip = inet_addr("10.0.0.2"); // LOCAL_IP
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 5. 填充 Layer 4: TCP Header (符合驗收規格)
    tcp->src_port = htons(52144);
    tcp->dst_port = htons(8080);
    tcp->seq = htonl(1000);
    tcp->ack = htonl(0);
    tcp->data_offset = (5 << 4); // 5 words = 20 bytes
    tcp->flags = TCP_SYN;        // SYN = 1, ACK = 0
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    // 6. 發送封包
    int len = sizeof(frame);
    printf("發送 TCP SYN 測試封包到 tap0...\n");
    printf("  Src Port : %u\n", ntohs(tcp->src_port));
    printf("  Dst Port : %u\n", ntohs(tcp->dst_port));
    printf("  SEQ      : %u\n", ntohl(tcp->seq));
    printf("  Flags    : 0x%02x (SYN = 1, ACK = 0)\n", tcp->flags);
    printf("  Window   : %u\n", ntohs(tcp->window));

    if (sendto(sockfd, frame, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto");
    } else {
        printf("成功送出 %d bytes 的 TCP 封包！\n", len);
    }

    close(sockfd);
    return 0;
}
