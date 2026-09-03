#include <stdio.h>
#include <string.h>

#include "arp_table.h"

struct arp_entry arp_table[ARP_TABLE_SIZE];

void arp_table_init(void)
{
    memset(arp_table, 0, sizeof(arp_table));
}

int arp_table_insert(const uint8_t *ip, const uint8_t *mac)
{
    // 若 IP 已存在則更新 MAC
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && memcmp(arp_table[i].ip, ip, 4) == 0) {
            memcpy(arp_table[i].mac, mac, 6);
            return 0;
        }
    }

    // 寫入第一個空位
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            memcpy(arp_table[i].ip, ip, 4);
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = 1;
            return 0;
        }
    }

    return -1; // 表滿
}

struct arp_entry *arp_table_lookup(const uint8_t *ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && memcmp(arp_table[i].ip, ip, 4) == 0) {
            return &arp_table[i];
        }
    }
    return NULL;
}

void arp_table_dump(void)
{
    printf("\nARP Table\n=========================\n");
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid) {
            printf("%u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                   arp_table[i].ip[0], arp_table[i].ip[1],
                   arp_table[i].ip[2], arp_table[i].ip[3],
                   arp_table[i].mac[0], arp_table[i].mac[1],
                   arp_table[i].mac[2], arp_table[i].mac[3],
                   arp_table[i].mac[4], arp_table[i].mac[5]);
        }
    }
    printf("\n");
    fflush(stdout);
}
