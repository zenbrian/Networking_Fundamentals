#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h> // 提供 rand() 與 srand()
#include <time.h>   // 提供 time() 作為隨機種子

#include "dns.h"

/* 將一般網址格式（如 google.com）轉換為 DNS 標籤格式（06 google 03 com 00）
 * 回傳值：編碼後佔用的總 byte 數
 */
int dns_encode_name(const char *domain, uint8_t *buffer)
{
    const char *start = domain;
    uint8_t *p = buffer;

    while (*start) {
        // 尋找下一個點 '.'
        const char *dot = strchr(start, '.');
        int len;

        if (dot) {
            len = dot - start; // 點之前的長度
        } else {
            len = strlen(start); // 最後一個段落（如 com）
        }

        // 防禦性檢查：單一 label 長度不能超過 63（RFC 1035 規定）
        if (len > 63) {
            return -1;
        }

        // 1. 寫入單詞長度
        *p++ = (uint8_t)len;

        // 2. 複製單詞字元
        memcpy(p, start, len);
        p += len;

        // 如果已經沒有點了，代表處理完畢
        if (!dot) {
            break;
        }

        // 移到下一個單詞起點
        start = dot + 1;
    }

    // 3. DNS 網域名稱必須以長度 0 結尾
    *p++ = 0;

    // 回傳總共寫入的位元組長度
    return p - buffer;
}

/* 組裝完整的 DNS 查詢封包 (Header + Question)
 * 回傳值：組裝完成的 DNS Query 總位元組數 (失敗回傳 -1)
 */
int dns_build_query(const char *domain, uint8_t *buf, size_t buf_size)
{
    // 防禦性檢查：至少要放得下 Header (12) + QTYPE/QCLASS (4)
    if (buf_size < sizeof(struct dns_header) + 4) {
        return -1;
    }

    memset(buf, 0, buf_size);

    // 1. 組裝 DNS Header (12 bytes)
    struct dns_header *dns = (struct dns_header *)buf;
    
    /* 隨機化 ID，避免猜測：
     * 這是關鍵！必須使用全 16-bit 的隨機值，
     * 讓你的 DNS Query 看起來像是真正的客戶端發出的封包。
     */
    uint16_t tx_id = (uint16_t)(rand() & 0xFFFF);
    dns->id = htons(tx_id); 
    dns->flags = htons(0x0100);     // 0x0100: 標準查詢 + Recursion Desired
    dns->qdcount = htons(1);        // 只有 1 個問題
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    // 2. 組裝 Question: QNAME (在 Header 之後)
    uint8_t *qname_ptr = buf + sizeof(struct dns_header);
    int name_len = dns_encode_name(domain, qname_ptr);
    if (name_len < 0) {
        return -1;
    }

    // 3. 組裝 Question: QTYPE (2 bytes) 與 QCLASS (2 bytes)
    uint8_t *qtrailer_ptr = qname_ptr + name_len;
    
    // 檢查 buffer 容量是否足夠
    if ((size_t)(sizeof(struct dns_header) + name_len + 4) > buf_size) {
        return -1;
    }

    // QTYPE = 1 (A Record, 查 IPv4)
    uint16_t qtype = htons(DNS_TYPE_A);
    memcpy(qtrailer_ptr, &qtype, sizeof(uint16_t));

    // QCLASS = 1 (IN, 網際網路)
    uint16_t qclass = htons(DNS_CLASS_IN);
    memcpy(qtrailer_ptr + sizeof(uint16_t), &qclass, sizeof(uint16_t));

    // 回傳整包封包總長度：Header(12) + Name + Type/Class(4)
    return sizeof(struct dns_header) + name_len + 4;
}


/* 解析 DNS Server 回傳的回覆封包，提取並印出 IPv4 位址 */
void dns_parse_response(const uint8_t *buffer, size_t len)
{
    // 1. 基本長度檢查：至少要有 12 bytes 的 Header
    if (len < sizeof(struct dns_header)) {
        printf("[DNS] Response too short!\n");
        return;
    }

    const struct dns_header *dns = (const struct dns_header *)buffer;
    uint16_t ancount = ntohs(dns->ancount);
    uint16_t qdcount = ntohs(dns->qdcount);

    printf("\n=== DNS Response ===\n");
    printf("Transaction ID : 0x%04x\n", ntohs(dns->id));
    printf("Flags          : 0x%04x\n", ntohs(dns->flags));
    printf("Questions      : %u\n", qdcount);
    printf("Answer RRs     : %u\n", ancount);

    if (ancount == 0) {
        printf("[DNS] No answers found.\n");
        return;
    }

    // 2. 移動指標，跳過 Question 區段
    const uint8_t *p = buffer + sizeof(struct dns_header);

    for (int i = 0; i < qdcount; i++) {
        // 跳過 QNAME：不斷往下找直到遇到 0x00 結尾
        while (p < buffer + len && *p != 0) {
            // 如果遇到指針壓縮 (0xc0 開頭)，佔 2 bytes
            if ((*p & 0xc0) == 0xc0) {
                p += 2;
                break;
            }
            p += (*p + 1); // 跳過長度 byte + 內容
        }
        if (*p == 0) {
            p++; // 跳過 0x00 結尾符
        }
        p += 4; // 跳過 QTYPE (2B) + QCLASS (2B)
    }

    // 3. 解析 Answer 區段
    printf("\n--- Resolved IP Addresses ---\n");
    for (int i = 0; i < ancount; i++) {
        if (p >= buffer + len) break;

        // 處理 Answer 中的 NAME (通常為 0xc0 開頭的 2-byte 指標)
        if ((*p & 0xc0) == 0xc0) {
            p += 2;
        } else {
            while (p < buffer + len && *p != 0) {
                p += (*p + 1);
            }
            if (*p == 0) p++;
        }

        if (p + 10 > buffer + len) break;

                uint16_t type = ntohs(*(uint16_t *)p);
        p += 2; // skip type

        uint16_t class = ntohs(*(uint16_t *)p); // 讀取 class
        p += 2; // skip class

        uint32_t ttl = ntohl(*(uint32_t *)p);
        p += 4; // skip ttl

        uint16_t rdlength = ntohs(*(uint16_t *)p);
        p += 2; // skip rdlength

        // 如果是 Type A (IPv4 位址) 且資料長度為 4 bytes
        if (type == DNS_TYPE_A && rdlength == 4) {
            const uint8_t *ip = p;
            printf("IPv4 Address : %u.%u.%u.%u (Class: %u [IN], TTL: %us)\n",
                   ip[0], ip[1], ip[2], ip[3], class, ttl);
        } else if (type == DNS_TYPE_CNAME) {
            printf("CNAME Record (Alias) [Class: %u, TTL: %us]\n", class, ttl);
        }
        p += rdlength; // 移動到下一筆 Answer
    }
    printf("=============================\n\n");

}
