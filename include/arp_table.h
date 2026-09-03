#ifndef ARP_TABLE_H
#define ARP_TABLE_H

#include <stdint.h>

#define ARP_TABLE_SIZE 32

struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
};

extern struct arp_entry arp_table[ARP_TABLE_SIZE];

void arp_table_init(void);
int arp_table_insert(const uint8_t *ip, const uint8_t *mac);
struct arp_entry *arp_table_lookup(const uint8_t *ip);
void arp_table_dump(void);

#endif
