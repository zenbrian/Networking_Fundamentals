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
    arp_table_init();

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
                    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
                    ipv4_print_header(ip);

                    // IPv4 Protocol Dispatcher
                    switch (ip->protocol) {
                        case IPPROTO_ICMP:
                            if (payload_len >= sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)) {
                                const struct icmp_hdr *icmp_in =
                                    (const struct icmp_hdr *)(payload + sizeof(struct ipv4_hdr));
                                size_t icmp_in_len = payload_len - sizeof(struct ipv4_hdr);

                                icmp_print_header(icmp_in, icmp_in_len);

                                // 如果是 Echo Request，建立並送出 Reply
                                if (icmp_in->type == ICMP_ECHO_REQUEST) {
                                    uint8_t reply[2048];
                                    memcpy(reply, buffer, n); // 複製收到的完整 Frame

                                    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
                                    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
                                    uint8_t *icmp_out = reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr);
                                    size_t icmp_len = n - (ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

                                    // 1. 交換 Ethernet MAC
                                    uint8_t temp_mac[ETH_ADDR_LEN];
                                    memcpy(temp_mac, eth_out->dst, ETH_ADDR_LEN);
                                    memcpy(eth_out->dst, eth_out->src, ETH_ADDR_LEN);
                                    memcpy(eth_out->src, temp_mac, ETH_ADDR_LEN);

                                    // 2. 交換 IPv4 來源與目的 IP，並更新 TTL 與 Checksum
                                    uint32_t temp_ip = ip_out->dst_ip;
                                    ip_out->dst_ip = ip_out->src_ip;
                                    ip_out->src_ip = temp_ip;
                                    ip_out->ttl = 64;
                                    ip_out->checksum = 0;
                                    ip_out->checksum = ipv4_checksum(ip_out, sizeof(struct ipv4_hdr));

                                    // 3. 修改 ICMP 為 Echo Reply 並重算 ICMP Checksum
                                    icmp_handle(icmp_out, icmp_len);

                                    // 4. 送出 Reply Frame 回 tap 介面 (TX)
                                    ssize_t sent = write(fd, reply, n);
                                    if (sent < 0) {
                                        perror("[ICMP] write to tap failed");
                                    } else {
                                        printf("[ICMP] Echo Reply sent (%ld bytes)\n", sent);
                                        fflush(stdout);
                                    }
                                }
                            }
                            break;
                        default:
                            break;
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
