#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "dns.h"

#define DNS_SERVER_IP "8.8.8.8" // Google Public DNS

int main(int argc, char *argv[])
{
    // 1. 取得使用者指定的網址（若無指定則預設為 google.com）
    const char *domain = "google.com";
    if (argc > 1) {
        domain = argv[1];
    }

    // 初始化隨機種子，讓每次發送的 Transaction ID 都真正隨機
    srand(time(NULL));

    printf("Querying DNS Server (%s:53) for: %s ...\n", DNS_SERVER_IP, domain);

    // 2. 組裝 DNS Query 封包
    uint8_t query_buf[512];
    int query_len = dns_build_query(domain, query_buf, sizeof(query_buf));
    if (query_len < 0) {
        fprintf(stderr, "Failed to build DNS query packet.\n");
        return 1;
    }

    // 3. 建立標準 UDP Socket
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    // 設定 3 秒超時，避免網路不通時程式卡死
    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // 設定目的地：8.8.8.8 的 Port 53
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DNS_PORT);
    dest.sin_addr.s_addr = inet_addr(DNS_SERVER_IP);

    // 4. 送出 UDP 封包
    ssize_t sent = sendto(sockfd, query_buf, query_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        perror("sendto");
        close(sockfd);
        return 1;
    }
    printf("Sent %zd bytes DNS query to %s\n", sent, DNS_SERVER_IP);

    // 5. 等待接收 DNS Server 的回覆
    uint8_t resp_buf[1024];
    ssize_t resp_len = recvfrom(sockfd, resp_buf, sizeof(resp_buf), 0, NULL, NULL);
    if (resp_len < 0) {
        perror("recvfrom (Timeout or Server unreachable)");
        close(sockfd);
        return 1;
    }

    printf("Received %zd bytes DNS reply!\n", resp_len);

    // 6. 呼叫我們親手寫的解析器，提取並印出答案
    dns_parse_response(resp_buf, resp_len);

    close(sockfd);
    return 0;
}