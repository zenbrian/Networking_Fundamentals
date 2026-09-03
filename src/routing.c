#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "routing.h"

static struct route route_table[ROUTE_TABLE_SIZE];
static int route_count = 0;

void routing_init(void)
{
    memset(route_table, 0, sizeof(route_table));
    route_count = 0;
}

int routing_add(uint32_t network, uint32_t netmask, uint32_t gateway)
{
    if (route_count >= ROUTE_TABLE_SIZE) {
        printf("[ROUTING] Table full, cannot add route\n");
        return -1;
    }

    route_table[route_count].network = network;
    route_table[route_count].netmask = netmask;
    route_table[route_count].gateway = gateway;
    route_count++;

    return 0;
}

struct route *routing_lookup(uint32_t destination)
{
    struct route *default_route = NULL;

    for (int i = 0; i < route_count; i++) {
        struct route *r = &route_table[i];

        // 若為 Default Route (0.0.0.0/0)，先記錄當備案
        if (r->netmask == 0 && r->network == 0) {
            default_route = r;
            continue;
        }

        // 核心運算：(目標 IP & 遮罩) 是否等於 該網段？
        if ((destination & r->netmask) == r->network) {
            return r; // 命中具體區網，優先返回！
        }
    }

    // 若特定網段都沒命中，才走 Default Route
    return default_route;
}

void routing_dump(void)
{
    printf("\n=== Routing Table ===\n");
    printf("%-18s %-18s %-18s\n", "Destination", "Netmask", "Gateway");
    printf("----------------------------------------------------------\n");

    for (int i = 0; i < route_count; i++) {
        struct route *r = &route_table[i];
        char net_str[INET_ADDRSTRLEN];
        char mask_str[INET_ADDRSTRLEN];
        char gw_str[32];       

        inet_ntop(AF_INET, &r->network, net_str, sizeof(net_str));
        inet_ntop(AF_INET, &r->netmask, mask_str, sizeof(mask_str));
        
        if (r->gateway == 0) {
            snprintf(gw_str, sizeof(gw_str), "0.0.0.0 (Direct)");
        } else {
            inet_ntop(AF_INET, &r->gateway, gw_str, sizeof(gw_str));
        }

        printf("%-18s %-18s %-18s\n", net_str, mask_str, gw_str);
    }
    printf("=====================\n\n");
}
