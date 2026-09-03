#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>

#include "ethernet.h"

int tun_alloc(char *dev)
{
    struct ifreq ifr;
    int fd;

    // 開啟 TUN/TAP 核心驅動
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));

    // TAP 模式 + 不附加額外資訊
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (dev && *dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    // 綁定 tap0
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
    unsigned char buffer[2048];

    int fd = tun_alloc(dev);

    printf("TAP device: %s\n", dev);
    printf("Waiting for Ethernet frame...\n");

    while (1) {

        int n = read(fd, buffer, sizeof(buffer));

        if (n < 0) {
            perror("read");
            break;
        }

        if (n < ETH_HEADER_LEN) {
            printf("Invalid Ethernet frame\n");
            continue;
        }

        struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;

        ethernet_print_header(eth);
        printf("Frame length: %d bytes\n\n", n);
    }

    close(fd);
    return 0;
}
