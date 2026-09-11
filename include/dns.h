#ifndef DNS_H
#define DNS_H

#include <stdint.h>
#include <stddef.h>

#define DNS_PORT 53

/* DNS 查詢類型 (QTYPE) */
#define DNS_TYPE_A     1    // 查詢 IPv4 位址
#define DNS_TYPE_CNAME 5    // 別名

/* DNS 查詢類別 (QCLASS) */
#define DNS_CLASS_IN   1    // 網際網路 (Internet)

/* DNS Header (RFC 1035) - 固定 12 Bytes */
struct dns_header
{
    uint16_t id;       // Transaction ID（識別碼，Client 隨機指定，Server 原樣帶回）
    uint16_t flags;    // 標誌（例如 0x0100 代表標準查詢且啟用遞迴查詢）
    uint16_t qdcount;  // 問題數量 (Question Count)
    uint16_t ancount;  // 回答數量 (Answer RRs Count)
    uint16_t nscount;  // 授權伺服器數量 (Authority RRs Count)
    uint16_t arcount;  // 額外記錄數量 (Additional RRs Count)
} __attribute__((packed));

/* 接下來要實作的核心函式宣告 */
int dns_encode_name(const char *domain, uint8_t *buffer);
int dns_build_query(const char *domain, uint8_t *buf, size_t buf_size);
void dns_parse_response(const uint8_t *buffer, size_t len);

#endif /* DNS_H */
