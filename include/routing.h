#ifndef ROUTING_H
#define ROUTING_H

#include <stdint.h>

#define ROUTE_TABLE_SIZE 16

struct route {
    uint32_t network; // 目標網段 (Network Byte Order)
    uint32_t netmask; // 子網路遮罩 (Network Byte Order)
    uint32_t gateway; // 下一跳 Gateway IP (0 代表本地直連)
};

// 初始化路由表（清空）
void routing_init(void);

// 新增一筆路由到路由表
int routing_add(uint32_t network, uint32_t netmask, uint32_t gateway);

// 查詢目的地 IP 該走哪一條路由
struct route *routing_lookup(uint32_t destination);

// 印出當前路由表（Debug 觀察用）
void routing_dump(void);

#endif
