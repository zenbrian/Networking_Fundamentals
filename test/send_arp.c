#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "config.h"
#include "ethernet.h"
#include "arp.h"

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

    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        exit(1);
    }

    strcpy(dev, ifr.ifr_name);
    return fd;
}

int main()
{
    char dev[IFNAMSIZ] = "tap0";
    int fd = tun_alloc(dev);

    unsigned char frame[ETH_HEADER_LEN + sizeof(struct arp_packet)];
    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HEADER_LEN);

    uint8_t bcast_mac[ETH_ADDR_LEN] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    memcpy(eth->dst, bcast_mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_ARP);

    uint8_t target_ip[4] = {10, 0, 0, 1};
    arp_build_request(arp, LOCAL_MAC, LOCAL_IP, target_ip);

    printf("Sending ARP Request:\n\nWho has %u.%u.%u.%u ?\n\nTell %u.%u.%u.%u\n\n",
           target_ip[0], target_ip[1], target_ip[2], target_ip[3],
           LOCAL_IP[0], LOCAL_IP[1], LOCAL_IP[2], LOCAL_IP[3]);

    ssize_t sent = write(fd, frame, sizeof(frame));
    if (sent < 0) {
        perror("write");
    }

    close(fd);
    return 0;
}
