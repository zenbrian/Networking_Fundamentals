#include <stdio.h>
#include <arpa/inet.h>
#include "routing.h"

void test_lookup(const char *ip_str)
{
    uint32_t dst = inet_addr(ip_str);
    struct route *r = routing_lookup(dst);

    printf("Lookup %-15s -> ", ip_str);
    if (r) {
        if (r->gateway == 0) {
            printf("[LOCAL ROUTE] Direct link (On-link)\n");
        } else {
            char gw_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &r->gateway, gw_str, sizeof(gw_str));
            printf("[GATEWAY] Forward via %s\n", gw_str);
        }
    } else {
        printf("[NO ROUTE] Drop packet\n");
    }
}

int main(void)
{
    printf("=== Testing Routing Table ===\n");

    // 1. 初始化路由表
    routing_init();

    // 2. 加入本地網段路由：10.0.0.0/24 (Direct, Gateway = 0)
    routing_add(
        inet_addr("10.0.0.0"),        //network
        inet_addr("255.255.255.0"),   //mask
        0                             //gateway
    );

    // 3. 加入預設路由 (Default Route)：0.0.0.0/0 via 10.0.0.1
    routing_add(
        inet_addr("0.0.0.0"),        //network
        inet_addr("0.0.0.0"),        //mask
        inet_addr("10.0.0.1")        //gateway
    );

    // 4. 印出當前路由表
    routing_dump();

    // 5. 測試各種不同目的地的尋路決策
    printf("--- Running Lookup Tests ---\n");
    test_lookup("10.0.0.2");    // 本機或同區網設備 -> 應該命中 10.0.0.0/24 (Direct)
    test_lookup("10.0.0.55");   // 同區網鄰居 -> 應該命中 10.0.0.0/24 (Direct)
    test_lookup("8.8.8.8");     // Google DNS (外網) -> 應該命中 Default Gateway (10.0.0.1)
    test_lookup("140.112.1.1"); // 台大 (外網) -> 應該命中 Default Gateway (10.0.0.1)

    return 0;
}
