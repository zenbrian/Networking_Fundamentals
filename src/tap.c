#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "ethernet.h"
#include "arp.h"
#include "arp_table.h"
#include "ipv4.h"
#include "icmp.h"
#include "checksum.h"
#include "routing.h"  // <-- 引入 routing 標頭檔
#include "config.h"   // <-- 引入 LOCAL_IP 定義

int tun_alloc(char *dev)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (dev && *dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        exit(1);
    }

    strcpy(dev, ifr.ifr_name);

    // 自動將 TAP 網卡設置為 UP | RUNNING 狀態
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) >= 0) {
            ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
            ioctl(sock, SIOCSIFFLAGS, &ifr);
        }
        close(sock);
    }

    return fd;
}

int main()
{
    char dev[IFNAMSIZ] = "tap0";
    unsigned char buffer[2048];

    int fd = tun_alloc(dev);
    
    // ARP table 初始化
    arp_table_init();
    
    // 路由表初始化
    routing_init();
    routing_add(inet_addr("10.0.0.0"), inet_addr("255.255.255.0"), 0); // 區網直連
    routing_add(inet_addr("0.0.0.0"), inet_addr("0.0.0.0"), inet_addr("10.0.0.1")); // 預設閘道
    routing_dump(); // 印出路由表

    printf("TAP device: %s (UP)\n", dev);
    printf("Ethernet header size: %lu bytes\n", sizeof(struct ethernet_hdr));
    printf("Waiting for Ethernet frame...\n");
    fflush(stdout);

    while (1) {
        int n = read(fd, buffer, sizeof(buffer));

        if (n < 0) {
            perror("read");
            break;
        }

        if (n < ETH_HEADER_LEN) {
            printf("Invalid Ethernet frame\n");
            fflush(stdout);
            continue;
        }

        struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;

        if (!ethernet_accept_frame(eth)) {
            printf("[DROP] Not for me\n");
            fflush(stdout);
            continue;
        }

        printf("[ACCEPT]\n");
        ethernet_print_header(eth);

        // Ethernet EtherType 分流器 (Dispatcher)
        const uint8_t *payload = buffer + ETH_HEADER_LEN;
        size_t payload_len = n - ETH_HEADER_LEN;

        switch (ntohs(eth->ethertype)) {
            case ETHERTYPE_ARP:
                arp_receive(fd, payload, payload_len);
                break;

            case ETHERTYPE_IPV4:
                if (payload_len >= sizeof(struct ipv4_hdr)) {
                    // 1. 去掉 const，因為我們要修改 ip->ttl 的值
                    struct ipv4_hdr *ip = (struct ipv4_hdr *)payload;
                    ipv4_print_header(ip);
                    // 2. 扣減 TTL，若歸零則丟棄封包
                    if (ipv4_decrement_ttl(ip) != 0) {
                        printf("[IPv4] TTL Expired\n");
                        icmp_send_time_exceeded(fd, buffer, n);
                        fflush(stdout);
                        break; // 跳出 switch，不往下處理 protocol
                    }
                    // --- 從這裡開始加入 Routing 判斷 ---
                    uint32_t my_ip = *(uint32_t *)LOCAL_IP; // 10.0.0.2
                    if (ip->dst_ip == my_ip) {
                        // 目的 IP 是我：Local Delivery 本地接收
                        printf("[IPv4] Local Delivery (for me)\n");
                        fflush(stdout);
                        // IPv4 Protocol Dispatcher
                        switch (ip->protocol) {
                            case IPPROTO_ICMP:
                                icmp_receive(fd, buffer, n);
                                break;
                            default:
                                break;
                        }
                    }else {
                        // 目的 IP 不是我：進入 Routing 路由查詢！
                        printf("[IPv4] Not for me -> Routing Lookup\n");
                        fflush(stdout);
                        struct route *r = routing_lookup(ip->dst_ip);
                        if (r == NULL) {
                            printf("[IPv4] No route -> DROP\n");
                            fflush(stdout);
                            break;
                        }
                        if (r->gateway == 0) {
                            printf("[IPv4] Route found: Direct delivery on local network\n");
                        } else {
                            char gw_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &r->gateway, gw_str, sizeof(gw_str));
                            printf("[IPv4] Route found: Forward via Gateway %s\n", gw_str);
                        }
                        fflush(stdout);
                        }
                }
                break;
                

            default:
                printf("Unknown EtherType: 0x%04x\n", ntohs(eth->ethertype));
                break;
        }

        printf("Frame length: %d bytes\n\n", n);
        fflush(stdout);
    }

    close(fd);
    return 0;
}
