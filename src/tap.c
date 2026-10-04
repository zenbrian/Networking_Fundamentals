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
#include "udp.h"
#include "tcp.h"
#include "socket.h"
#include "http.h"
#include "controller.h"

static int global_tap_fd = -1;

int get_tap_fd(void)
{
    return global_tap_fd;
}

/* 第一個 UDP 應用程式：收到什麼就印出什麼，並觸發 udp_send 回應 9999 Port */
void udp_echo_app(const uint8_t *data, size_t len)
{
    printf("\n=== UDP APP ===\n");
    // 把資料印到螢幕上 (fwrite 可以安全輸出任何 byte，不怕中間有 \0)
    fwrite(data, 1, len, stdout);
    printf("===============\n\n");
    fflush(stdout);

    // 觸發發送！主動送一包 "Hello UDP" 到 10.0.0.1 的 9999 Port！
    printf("[APP] Triggering udp_send to 10.0.0.1:9999...\n");
    udp_send(global_tap_fd, 8080, inet_addr("10.0.0.1"), 9999, (const uint8_t *)"Hello UDP\n", 10);
}

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


void net_init(void)
{
    char dev[IFNAMSIZ] = "tap0";
    int fd = tun_alloc(dev);
    global_tap_fd = fd;

    arp_table_init();
    routing_init();
    routing_add(inet_addr("10.0.0.0"), inet_addr("255.255.255.0"), 0);
    routing_add(inet_addr("0.0.0.0"), inet_addr("0.0.0.0"), inet_addr("10.0.0.1"));
    udp_init();
    udp_bind(8080, udp_echo_app);
    tcp_init();

    printf("TAP device: %s (UP)\n", dev);
    printf("TCP/IP Stack Initialized.\n");
    fflush(stdout);
}

int net_poll(void)
{
    int fd = global_tap_fd;
    if (fd < 0) return -1;

    unsigned char buffer[2048];
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms 超時

    int sel = select(fd + 1, &fds, NULL, NULL, &tv);

    // 每隔 100ms 醒來一次檢查重傳與 TIME_WAIT
    tcp_check_time_wait();
    tcp_check_retransmission(fd);

    if (sel <= 0) {
        return 0; // 超時或被中斷，結束這次 poll
    }

    int n = read(fd, buffer, sizeof(buffer));
    if (n < 0) {
        perror("read");
        return -1;
    }

    if (n < ETH_HEADER_LEN) {
        printf("Invalid Ethernet frame\n");
        fflush(stdout);
        return 0;
    }

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    if (!ethernet_accept_frame(eth)) {
        printf("[DROP] Not for me\n");
        fflush(stdout);
        return 0;
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
                struct ipv4_hdr *ip = (struct ipv4_hdr *)payload;
                ipv4_print_header(ip);

                if (ipv4_decrement_ttl(ip) != 0) {
                    printf("[IPv4] TTL Expired\n");
                    icmp_send_time_exceeded(fd, buffer, n);
                    fflush(stdout);
                    break;
                }

                uint32_t my_ip = *(uint32_t *)LOCAL_IP; // 10.0.0.2
                if (ip->dst_ip == my_ip) {
                    printf("[IPv4] Local Delivery (for me)\n");
                    fflush(stdout);
                    switch (ip->protocol) {
                        case IPPROTO_ICMP:
                            icmp_receive(fd, buffer, n);
                            break;
                        case IPPROTO_UDP:
                            udp_receive(fd, buffer, n);
                            break;
                        case IPPROTO_TCP:
                            tcp_receive(fd, buffer, n);
                            break;
                        default:
                            break;
                    }
                } else {
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
    return 1; // 成功處理了一個封包！
}


int main()
{
    // 1. 初始化底層網路
    net_init();

    // 2. 用你的 Socket API 架設伺服器
    struct socket *listener = socket_create();
    socket_bind(listener, 8080);
    socket_listen(listener);

    printf("\n========================================\n");
    printf("   HTTP Web Server Running on Port 8080   \n");
    printf("========================================\n\n");

    // 3. 經典 HTTP Web Server 主迴圈
    while (1) {
        struct socket *conn = socket_accept(listener);
        if (!conn) continue;

        uint8_t buf[1024];
        int n = socket_recv(conn, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("\n[http Server] Received: %s\n", buf);

            struct http_request req;
            if (http_parse_request((char *)buf, &req) == 0) {
                http_print_request(&req);
                char resp[65536];

                // 由 Controller 模組統一進行路由分流派送
                int resp_len = http_route_dispatch(&req, resp, sizeof(resp));

                if (resp_len > 0) {
                    socket_send(conn, (const uint8_t *)resp, resp_len);
                }
            }
            
        }

        socket_close(conn);
    }

    return 0;
}

