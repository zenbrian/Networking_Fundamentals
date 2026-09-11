# Day 15：第一個真實網路應用 — Mini DNS Client

昨天（Day 14）我們完成了由上而下的主動發送路徑，成功將 UDP 封包射出虛擬網卡：
```text
Application ──► UDP ──► IPv4 ──► Ethernet ──► TAP
```

然而在過去 14 天中，我們每一次測試都是手動把目的 IP（如 `10.0.0.1`、`10.0.0.2`）寫死在程式碼或指令裡。

今天（Day 15），我們正式打造了第一個建立在 UDP 之上的**應用層協定（L7 Application Layer）—— Mini DNS Client**！
```text
Application (DNS Client)
           │
          DNS
           │
          UDP (Port 53)
           │
          IPv4
           │
        Ethernet
           │
          TAP / Host Network
```

我們的網路堆疊第一次具備了將人類可讀的網域名稱（`google.com`）透過標準 RFC 1035 通訊協定，向全世界最大的公共伺服器（Google DNS `8.8.8.8:53`）進行即時查詢，並成功解析出真實的網際網路 IP 位址！

---

# 今日學習目標與成果

- [x] **理解 DNS 系統本質與運作流程**：釐清電腦本地沒有全世界對照表時，如何透過預設 DNS 伺服器突破「雞生蛋、蛋生雞」的連網第一步。
- [x] **掌握 RFC 1035 標準 12-Byte DNS Header**：定義交易識別碼（ID）、標誌（Flags）及各區段計數器（QDCOUNT, ANCOUNT 等）。
- [x] **深入剖析 16-bit Flags 儀表板**：逐 bit 拆解 QR、Opcode、RD、RA 與 RCODE，理解發送端 `0x0100` 與接收端 `0x8180` 的精確意義。
- [x] **實作網域名稱長度標籤編碼器（`dns_encode_name`）**：將一般網址轉換為 Length-prefixed 格式（如 `google.com` ➔ `06 google 03 com 00`），並以單元測試獨立驗證。
- [x] **動態組裝完整的 DNS Question（`dns_build_query`）**：利用指標位移 `qtrailer_ptr = qname_ptr + name_len`，安全填充 QTYPE (Type A) 與 QCLASS (Class IN)。
- [x] **擬真工業級隨機 Transaction ID**：採用 `rand() & 0xFFFF` 避免可預測 ID，防範經典的 DNS 快取毒害攻擊（DNS Cache Poisoning）。
- [x] **解析 DNS 回覆與壓縮指標（Compression Pointer `0xc0`）**：實作 `dns_parse_response` 跳過問題區段、處理 2-byte 指標跳轉、解析 Type A 與 CNAME，精確提取 4-byte IPv4 位址。
- [x] **實測見證 DNS 負載平衡（Round-Robin）與高可用性**：成功解析 `google.com`，驗證程式穩定處理單筆 44 Bytes 及多筆 6 組 IP（124 Bytes）的回覆。

---

# 核心概念深入剖析

### 1. DNS 是什麼？為什麼電腦需要它？

* **電腦與網路設備只認數字（IP 位址）**：
  在 L3 IPv4 標頭中，`dst_ip` 必須是 32-bit 的純數字（如 `142.250.204.46`）。如果沒有 IP，IPv4 標頭填不出來，路由器根本不知道往哪裡送。
* **人類只記得住文字（網域名稱，Domain Name）**：
  人類難以記憶海量的冰冷數字，但能輕易記住 `google.com`。
* **DNS（Domain Name System）就是網路世界的查號台**：
  連網的第一動，永遠是先問查號台「請問這個網址對應的 IP 是多少？」，拿到 IP 後才正式發動連線。

---

### 2. 電腦一開機沒有對照表，怎麼拿到 Google IP？

> **「雞生蛋、蛋生雞」的疑惑：如果我連 Google IP 都不知道，我要怎麼上網查它？**

答案是：**個人電腦裡本來就沒有 Google 的表！但電腦必須預先知道「查號台的純數字 IP」！**

```text
[你的電腦 / DNS Client]                             [Google DNS: 8.8.8.8]
        │                                                     │
        │ 1. 電腦開機時透過 DHCP 取得查號台純數字 IP           │
        │    (例如 8.8.8.8，完全不需要先查網址)               │
        │                                                     │
        │ 2. 打包 UDP 封包送到 8.8.8.8:53                     │
        │    Payload 詢問: "請問 google.com 是多少？"         │
        │ ──────────────────────────────────────────────────> │
        │                                                     │ 3. 8.8.8.8 翻閱全球資料庫
        │                                                     │    找到: 142.250.204.46
        │                                                     │
        │ 4. 8.8.8.8 回傳 UDP 回信                            │
        │    Payload 告知: "142.250.204.46"                   │
        │ <────────────────────────────────────────────────── │
        ▼
   拿到 IP！填入 IPv4 Header 開始上網！
```

---

### 3. DNS Header 結構與 6 大欄位職責

DNS 請求（Query）與回覆（Reply）**共用同一個固定 12 Bytes 的 Header**（RFC 1035）：

```text
 0  1  2  3  4  5  6  7  8  9 10 11 12 13 14 15 (bits)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                      ID                       |  -> 16 bits (Transaction ID)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|QR|   Opcode  |AA|TC|RD|RA|   Z    |   RCODE   |  -> 16 bits (Flags 標誌)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    QDCOUNT                    |  -> 16 bits (問題數量)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    ANCOUNT                    |  -> 16 bits (回答數量)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    NSCOUNT                    |  -> 16 bits (授權伺服器數量)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                    ARCOUNT                    |  -> 16 bits (額外記錄數量)
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

1. **`id`（交易識別碼）**：因為 UDP 是無狀態的（Stateless），Client 發送時自訂一個隨機 ID（存根）。Server 回覆時**必須原樣抄回**，Client 藉此比對這是哪一筆查詢的回信。
2. **`flags`（控制旗標）**：指示是 Query 還是 Response、是否要求遞迴查詢、是否有錯誤。
3. **`qdcount`（問題數）**：Client 發問時設為 1。
4. **`ancount`（答案數）**：發送時填 0；接收時由 Server 告知有幾筆 IP 答案。
5. **`nscount` / `arcount`**：授權與額外記錄數量，發送時為 0。

---

### 4. 16-bit Flags 儀表板深層解析

DNS 的 `flags` 是整個協定的核心指揮官：

| 欄位 | 位元數 | 功能說明 | 發送 Query (`0x0100`) | 接收 Response (`0x8180`) |
| :--- | :---: | :--- | :---: | :---: |
| **QR** | 1 bit | 0 = Query（問題），1 = Response（回答） | `0` | `1` |
| **Opcode** | 4 bits | 0 = 標準查詢（Standard Query） | `0000` | `0000` |
| **AA** | 1 bit | 授權回答（Authoritative Answer） | `0` | `0` |
| **TC** | 1 bit | 截斷標誌（超過 UDP 512 bytes 需改用 TCP） | `0` | `0` |
| **RD** | 1 bit | **遞迴查詢期望（Recursion Desired）** | **`1`** | **`1`** |
| **RA** | 1 bit | **伺服器支援遞迴（Recursion Available）** | `0` | **`1`** |
| **Z** | 3 bits | 保留位元，必須為 0 | `000` | `000` |
| **RCODE** | 4 bits | 回應狀態碼：`0` = NoError，`3` = NXDOMAIN | `0000` | `0000` (成功) |

* **為什麼我們發送時填 `0x0100`？**
  二進位為 `0000 0001 0000 0000`，代表：QR=0（發問）且 **RD=1**。告訴 8.8.8.8：「拜託幫我跑腿查到底，直接回我最終 IP，不要叫我再跑去問其他伺服器！」
* **為什麼 Google 回信時是 `0x8180`？**
  二進位為 `1000 0001 1000 0000`，代表：**QR=1（回信）**、**RD=1**、**RA=1（已幫你遞迴查完）**、**RCODE=0（查詢成功無誤）**！

---

### 5. 網域名稱長度標籤編碼（Length-prefixed Label）

DNS 封包不採用句點（`.`）與結尾 `\0`，而是採用長度標籤編碼格式：
* 每個單詞前方放 1-byte 表示該單詞的字元數。
* 整個網址結尾必須是一個 `0x00`（長度為 0 的根標籤）。

以 `google.com` 為例：
```text
人類閱讀:    g  o  o  g  l  e  .  c  o  m
長度切分:  [6] g  o  o  g  l  e [3] c  o  m [0]
Hex 位元組: 06 67 6f 6f 67 6c 65 03 63 6f 6d 00
總長度   : 1 + 6 + 1 + 3 + 1 = 12 Bytes
```

---

### 6. 組裝 Question Section 與指標位移（`qtrailer_ptr`）

因為 `QNAME` 的長度隨網址而異（`google.com` 是 12 bytes，其他網址可能 30 bytes），無法用固定結構描述，必須使用動態指標計算：

```text
記憶體起始: buf
│
▼
┌──────────────────┬───────────────────────────────┬──────────┬──────────┐
│   dns_header     │             QNAME             │  QTYPE   │  QCLASS  │
│   (固定 12 B)    │           (動態變長！)        │  (2 B)   │  (2 B)   │
└──────────────────┴───────────────────────────────┴──────────┴──────────┘
▲                  ▲                               ▲
│                  │                               │
buf                qname_ptr                       qtrailer_ptr
                   (buf + 12)                      (qname_ptr + name_len)
```

1. 在 `qtrailer_ptr = qname_ptr + name_len;` 填入 `QTYPE = htons(1)`（Type A 查詢 IPv4）。
2. 緊接著在 `qtrailer_ptr + 2` 填入 `QCLASS = htons(1)`（Class IN 網際網路）。

---

### 7. 隨機 Transaction ID 的資安意義

* **教學寫死 `0x1234`**：便於 Wireshark 抓包除錯。
* **工業級實作隨機 ID（`rand() & 0xFFFF`）**：
  防範 **DNS 快取毒害（DNS Cache Poisoning / Kaminsky 攻擊）**。若 ID 固定或有規律，駭客可在真實 DNS 回覆前搶先發送偽造的回應封包（例如將銀行網址導向釣魚 IP）。隨機 16-bit ID 結合隨機 Client UDP Port 使攻擊者幾乎無法猜中！

---

### 8. 解析 DNS 回覆與壓縮指標（`0xc0`）

DNS 回應封包中，網域名稱通常不會重複拼寫，而是採用 **壓縮指標（Compression Pointer）**：
* 若位元組開頭 2 bits 為 `11`（即十六進位 `0xc0`），代表這是一個 **2-Byte 指標**，指向封包前面出現過的網址名稱。
* 解析 Answer 遇到 `(*p & 0xc0) == 0xc0` 時，直接將指標前進 2 bytes（`p += 2`）即可略過名稱！

```text
Answer Resource Record 記憶體排列：
┌──────────┬──────────┬──────────┬──────────┬──────────┬──────────┐
│   NAME   │   TYPE   │  CLASS   │   TTL    │ RDLENGTH │  RDATA   │
│ (指標2B) │  (2 B)   │  (2 B)   │  (4 B)   │  (2 B)   │ (IP, 4B) │
└──────────┴──────────┴──────────┴──────────┴──────────┴──────────┘
▲
│
指標 p 循序解析並前進...
```

* **指標前進的嚴謹性（為什麼 `p += 2; // skip class` 絕不能漏？）**：
  雖然我們只關心 TYPE（是否為 Type A）與 IP 資料，但 `CLASS` 欄位實實在在佔用了封包中的 2 個 bytes。若少跳 2 個 bytes，後面的 `TTL`、`RDLENGTH` 與 `RDATA`（真實 IP）記憶體位移將全面錯位，讀出的 IP 將淪為亂碼！

---

### 9. 現象觀察：為什麼 `google.com` 回覆 6 組 IP？

當我們執行 `./dns_client google.com` 時，Server 回傳了 6 組 IP：
1. **負載平衡（DNS Round-Robin / Load Balancing）**：
   Google 每秒承受數千萬次造訪，無法依賴單一主機。DNS 伺服器回傳一整組伺服器叢集（Cluster）IP，由客戶端分散連線。
2. **高可用性（High Availability / Failover）**：
   若第一組 IP 發生斷線或維護，客戶端可無縫自動切換至第二組 IP 繼續連線。
3. **程式碼健壯性證明**：
   我們的解析迴圈 `for (int i = 0; i < ancount; i++)` 搭配 `p += rdlength;` 精準遍歷了全部 6 筆記錄，封包長度暴增至 124 bytes 依然穩定拆解無誤！

---

# 關鍵程式碼實作

### 1. `include/dns.h`：DNS 標頭與常數定義

```c
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
    uint16_t id;       // Transaction ID
    uint16_t flags;    // Flags
    uint16_t qdcount;  // Questions Count
    uint16_t ancount;  // Answer RRs Count
    uint16_t nscount;  // Authority RRs Count
    uint16_t arcount;  // Additional RRs Count
} __attribute__((packed));

/* 核心函式介面 */
int dns_encode_name(const char *domain, uint8_t *buffer);
int dns_build_query(const char *domain, uint8_t *buf, size_t buf_size);
void dns_parse_response(const uint8_t *buffer, size_t len);

#endif /* DNS_H */
```

---

### 2. `src/dns.c`：編碼、組裝與回覆解析實作

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h> // 提供 rand() 與 srand()
#include <time.h>   // 提供 time() 作為隨機種子

#include "dns.h"

/* 將一般網址格式（如 google.com）轉換為 DNS 標籤格式（06 google 03 com 00） */
int dns_encode_name(const char *domain, uint8_t *buffer)
{
    const char *start = domain;
    uint8_t *p = buffer;

    while (*start) {
        const char *dot = strchr(start, '.');
        int len;

        if (dot) {
            len = dot - start;
        } else {
            len = strlen(start);
        }

        if (len > 63) {
            return -1;
        }

        *p++ = (uint8_t)len;
        memcpy(p, start, len);
        p += len;

        if (!dot) {
            break;
        }
        start = dot + 1;
    }

    *p++ = 0; // 結尾 0x00
    return p - buffer;
}

/* 組裝完整的 DNS 查詢封包 (Header + Question) */
int dns_build_query(const char *domain, uint8_t *buf, size_t buf_size)
{
    if (buf_size < sizeof(struct dns_header) + 4) {
        return -1;
    }

    memset(buf, 0, buf_size);

    // 1. 組裝 DNS Header (12 bytes)
    struct dns_header *dns = (struct dns_header *)buf;
    uint16_t tx_id = (uint16_t)(rand() & 0xFFFF); // 隨機 Transaction ID
    dns->id = htons(tx_id); 
    dns->flags = htons(0x0100);     // 0x0100: 標準查詢 + Recursion Desired
    dns->qdcount = htons(1);        // 1 個問題
    dns->ancount = 0;
    dns->nscount = 0;
    dns->arcount = 0;

    // 2. 組裝 Question: QNAME
    uint8_t *qname_ptr = buf + sizeof(struct dns_header);
    int name_len = dns_encode_name(domain, qname_ptr);
    if (name_len < 0) {
        return -1;
    }

    // 3. 組裝 Question: QTYPE (2 bytes) 與 QCLASS (2 bytes)
    uint8_t *qtrailer_ptr = qname_ptr + name_len;
    if ((size_t)(sizeof(struct dns_header) + name_len + 4) > buf_size) {
        return -1;
    }

    uint16_t qtype = htons(DNS_TYPE_A);
    memcpy(qtrailer_ptr, &qtype, sizeof(uint16_t));

    uint16_t qclass = htons(DNS_CLASS_IN);
    memcpy(qtrailer_ptr + sizeof(uint16_t), &qclass, sizeof(uint16_t));

    return sizeof(struct dns_header) + name_len + 4;
}

/* 解析 DNS 回覆封包，提取並印出 IPv4 位址 */
void dns_parse_response(const uint8_t *buffer, size_t len)
{
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

    // 跳過 Question 區段
    const uint8_t *p = buffer + sizeof(struct dns_header);
    for (int i = 0; i < qdcount; i++) {
        while (p < buffer + len && *p != 0) {
            if ((*p & 0xc0) == 0xc0) {
                p += 2;
                break;
            }
            p += (*p + 1);
        }
        if (*p == 0) {
            p++;
        }
        p += 4; // 跳過 QTYPE (2B) + QCLASS (2B)
    }

    // 解析 Answer 區段
    printf("\n--- Resolved IP Addresses ---\n");
    for (int i = 0; i < ancount; i++) {
        if (p >= buffer + len) break;

        // 處理 Answer 中的 NAME (壓縮指標 0xc0)
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

        uint16_t class = ntohs(*(uint16_t *)p);
        p += 2; // skip class

        uint32_t ttl = ntohl(*(uint32_t *)p);
        p += 4; // skip ttl

        uint16_t rdlength = ntohs(*(uint16_t *)p);
        p += 2; // skip rdlength

        if (type == DNS_TYPE_A && rdlength == 4) {
            const uint8_t *ip = p;
            printf("IPv4 Address : %u.%u.%u.%u (Class: %u [IN], TTL: %us)\n",
                   ip[0], ip[1], ip[2], ip[3], class, ttl);
        } else if (type == DNS_TYPE_CNAME) {
            printf("CNAME Record (Alias) [Class: %u, TTL: %us]\n", class, ttl);
        }

        p += rdlength; // 精準位移至下一筆 Answer
    }
    printf("=============================\n\n");
}
```

---

### 3. `test/dns_client.c`：客戶端發送與接收驗收工具

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "dns.h"

#define DNS_SERVER_IP "8.8.8.8"

int main(int argc, char *argv[])
{
    const char *domain = "google.com";
    if (argc > 1) {
        domain = argv[1];
    }

    srand(time(NULL));

    printf("Querying DNS Server (%s:53) for: %s ...\n", DNS_SERVER_IP, domain);

    uint8_t query_buf[512];
    int query_len = dns_build_query(domain, query_buf, sizeof(query_buf));
    if (query_len < 0) {
        fprintf(stderr, "Failed to build DNS query packet.\n");
        return 1;
    }

    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct timeval tv = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(DNS_PORT);
    dest.sin_addr.s_addr = inet_addr(DNS_SERVER_IP);

    ssize_t sent = sendto(sockfd, query_buf, query_len, 0,
                          (struct sockaddr *)&dest, sizeof(dest));
    if (sent < 0) {
        perror("sendto");
        close(sockfd);
        return 1;
    }
    printf("Sent %zd bytes DNS query to %s\n", sent, DNS_SERVER_IP);

    uint8_t resp_buf[1024];
    ssize_t resp_len = recvfrom(sockfd, resp_buf, sizeof(resp_buf), 0, NULL, NULL);
    if (resp_len < 0) {
        perror("recvfrom (Timeout or Server unreachable)");
        close(sockfd);
        return 1;
    }

    printf("Received %zd bytes DNS reply!\n", resp_len);

    dns_parse_response(resp_buf, resp_len);

    close(sockfd);
    return 0;
}
```

---

# 實戰驗收與位元組級精準驗算

### 實測終端機輸出

```text
user@MSI:/mnt/c/Users/zenboen/Tutorial/Networking_Fundamentals$ make dns_client
cc -Wall -Wextra -Iinclude test/dns_client.c src/dns.c -o dns_client

user@MSI:/mnt/c/Users/zenboen/Tutorial/Networking_Fundamentals$ ./dns_client google.com
Querying DNS Server (8.8.8.8:53) for: google.com ...
Sent 28 bytes DNS query to 8.8.8.8
Received 124 bytes DNS reply!

=== DNS Response ===
Transaction ID : 0x73d0
Flags          : 0x8180
Questions      : 1
Answer RRs     : 6

--- Resolved IP Addresses ---
IPv4 Address : 142.250.157.100 (Class: 1 [IN], TTL: 121s)
IPv4 Address : 142.250.157.102 (Class: 1 [IN], TTL: 121s)
IPv4 Address : 142.250.157.113 (Class: 1 [IN], TTL: 121s)
IPv4 Address : 142.250.157.138 (Class: 1 [IN], TTL: 121s)
IPv4 Address : 142.250.157.101 (Class: 1 [IN], TTL: 121s)
IPv4 Address : 142.250.157.139 (Class: 1 [IN], TTL: 121s)
=============================
```

---

### 位元組級精準驗算（Byte-by-Byte Breakdown）

#### 1. 發送端 Query 總長度驗算：精準 28 Bytes

| 區段 | 欄位內容 | 長度 (Bytes) | 累計長度 | 說明 |
| :--- | :--- | :---: | :---: | :--- |
| **DNS Header** | ID, Flags, Counts | **12** | 12 | Transaction ID(2) + Flags(2) + QDCOUNT(2) + 其餘Counts(6) |
| **QNAME** | `06 google 03 com 00` | **12** | 24 | `[6]google`(7) + `[3]com`(4) + `\0`(1) |
| **Question Trailer**| QTYPE + QCLASS | **4** | **28** | `QTYPE=1`(2) + `QCLASS=1`(2) |

$$\text{總長度} = 12 \text{ (Header)} + 12 \text{ (QNAME)} + 4 \text{ (Trailer)} = 28 \text{ Bytes (100\% 吻合)}$$

---

#### 2. 接收端 Reply 總長度驗算：精準 124 Bytes

| 區段 | 欄位內容 | 長度 (Bytes) | 累計長度 | 說明 |
| :--- | :--- | :---: | :---: | :--- |
| **DNS Header** | ID, Flags(0x8180), Counts | **12** | 12 | 包含 `ANCOUNT = 6` |
| **Question Section** | 原樣抄回的問題 | **16** | 28 | QNAME(12) + QTYPE(2) + QCLASS(2) |
| **Answer RRs (共 6 筆)** | 6 筆 IPv4 A 記錄 | **96** ($16 \times 6$) | **124** | 每一筆 A 記錄佔 16 Bytes：<br>• Name 指標 `0xc00c` (2B)<br>• Type=1 (2B)<br>• Class=1 (2B)<br>• TTL (4B)<br>• RDLength=4 (2B)<br>• RDATA IPv4 (4B) |

$$\text{總長度} = 12 \text{ (Header)} + 16 \text{ (Question)} + (16 \times 6) \text{ (6 筆 Answer)} = 124 \text{ Bytes (100\% 吻合)}$$

---

# 目前完成的 Protocol Stack

```text
Application Layer
       │
      ├── DNS (Mini DNS Client - Day 15)  [Port 53]
      │
Transport Layer
       │
      ├── UDP (Port Multiplexing & Dispatcher - Day 12~14)
      │
Network Layer
       │
      ├── ICMP (Echo Request / Reply / Time Exceeded - Day 07~10)
      │
      ├── IPv4 (Header, Checksum, Decrement TTL, Routing - Day 05~11)
      │
      └── ARP  (ARP Request / Reply & ARP Table Cache - Day 03~06)
Link Layer
       │
      └── Ethernet (EtherType Dispatcher, MAC Filtering - Day 01~02)
Hardware / Virtual Interface
       │
      └── Linux TAP Device (/dev/net/tun)
```

---

# Day 16 預告：進入整個課程的最核心 — TCP

完成第一個 L7 應用服務後，明天我們即將邁入電腦網路中最深奧、也是全世界流量承載量最大的傳輸層霸主：

```text
TCP (Transmission Control Protocol)
```

我們不會一開始就做複雜的 Handshake，而是先打穩基本功：
1. **TCP Header 結構與 RFC 793 標準**（固定 20 Bytes 起跳，包含 Options 欄位）
2. **關鍵序號機制**：理解 `Sequence Number` 與 `Acknowledgement Number` 如何實現可靠傳輸與重傳。
3. **TCP 控制旗標（Flags）全景圖**：
   * `SYN`（同步連線）
   * `ACK`（確認收訖）
   * `FIN`（結束連線）
   * `RST`（強制重置）
   * `PSH`（立即推送）
   * `URG`（緊急指標）
4. **滑動視窗（Window Size）與流量控制概念**。

你將第一次親眼看到網際網路每天運作幾千億次的連線心臟！
