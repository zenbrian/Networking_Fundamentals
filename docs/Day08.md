# Day 8：ICMP 基礎 — 看懂 Ping 封包

昨天我們完成了 IPv4 Header 的 Checksum 驗證機制。
今天我們開始解析 IPv4 的第一個上層協定 ── **ICMP (Internet Control Message Protocol)**！

```text
Ethernet
    ↓
IPv4 (Protocol = 1)
    ↓
ICMP (Echo Request / Echo Reply)
```

---

# 今日學習目標與成果

完成以下功能：
- [x] 建立 `include/icmp.h` 定義 ICMP 標頭結構 `struct icmp_hdr` (大小為 8 bytes)
- [x] 實作 `src/icmp.c` 解析 Type, Code, Checksum, Identifier, Sequence
- [x] 實作 `icmp_verify_checksum()` 驗證 ICMP Checksum
- [x] 升級 `src/tap.c` 的 IPv4 Dispatcher：依據 `ip->protocol == IPPROTO_ICMP` 分發至 ICMP 解析器
- [x] 建立 `tests/send_icmp.c` 發送測試用 ICMP Echo Request (Type=8, Code=0, Id=1234, Seq=1)
- [x] 成功通過發送與接收驗證，Checksum 驗證 OK

---

# 核心概念與架構解析

### 1. ICMP Header 結構 (RFC 792)

ICMP 標頭大小為 **8 Bytes**：

```text
0               8              16                             31
+---------------+---------------+------------------------------+
| Type          | Code          | Checksum                     |
+---------------+---------------+------------------------------+
| Identifier (ID)               | Sequence Number (序列號)     |
+---------------+---------------+------------------------------+
```

常見 ICMP Type 意義：
- **Type 0 (Echo Reply)**：Ping 回應
- **Type 3 (Destination Unreachable)**：目標不可達
- **Type 8 (Echo Request)**：Ping 請求
- **Type 11 (Time Exceeded)**：TTL 逾時

### 2. 雙重 Checksum 職責分工

在 Ping 封包中，包含兩個不同的 Checksum：
- **IPv4 Checksum (`0x66de`)**：保護 L3 IPv4 Header (20 Bytes)。
- **ICMP Checksum (`0xf32c`)**：保護 L4/上層 ICMP 封包 (8 Bytes)。

---

# 今日新增與更新程式碼

### 1. `include/icmp.h` & `src/icmp.c`

```c
// include/icmp.h
#ifndef ICMP_H
#define ICMP_H

#include <stdint.h>
#include <stddef.h>

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

void icmp_print_header(const struct icmp_hdr *icmp, size_t length);
int icmp_verify_checksum(const void *icmp, size_t length);

#endif
```

```c
// src/icmp.c
#include <stdio.h>
#include <arpa/inet.h>

#include "icmp.h"
#include "checksum.h"

int icmp_verify_checksum(const void *icmp, size_t length)
{
    uint16_t calc_cksum = ipv4_checksum(icmp, length);
    return (calc_cksum == 0);
}

void icmp_print_header(const struct icmp_hdr *icmp, size_t length)
{
    int is_correct = icmp_verify_checksum(icmp, length);

    printf("\n");
    printf("ICMP Packet\n");
    printf("-------------------\n");
    printf("Type       : %u\n", icmp->type);
    printf("Code       : %u\n", icmp->code);
    printf("Checksum   : 0x%04x (%s)\n", ntohs(icmp->checksum), is_correct ? "OK" : "FAIL");
    printf("Identifier : %u\n", ntohs(icmp->identifier));
    printf("Sequence   : %u\n", ntohs(icmp->sequence));
    printf("\n");
}
```

### 2. IPv4 Dispatcher 升級 (`src/tap.c`)

```c
    case ETHERTYPE_IPV4:
        if (payload_len >= sizeof(struct ipv4_hdr)) {
            const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
            ipv4_print_header(ip);

            // IPv4 Protocol Dispatcher
            switch (ip->protocol) {
                case IPPROTO_ICMP:
                    if (payload_len >= sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)) {
                        const struct icmp_hdr *icmp =
                            (const struct icmp_hdr *)(payload + sizeof(struct ipv4_hdr));

                        icmp_print_header(icmp, payload_len - sizeof(struct ipv4_hdr));
                    }
                    break;
                default:
                    break;
            }
        }
        break;
```

### 3. 測試發送程式 (`tests/send_icmp.c`)

```c
    // ICMP Header 填寫與 Checksum 計算
    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->identifier = htons(1234);
    icmp->sequence = htons(1);

    icmp->checksum = ipv4_checksum(icmp, sizeof(struct icmp_hdr));
```

---

# 編譯與測試指令

```bash
# 編譯接收端 network
gcc -Iinclude src/tap.c src/ethernet.c src/arp.c src/arp_table.c src/ipv4.c src/icmp.c src/checksum.c -o network

# 編譯發送端 send_icmp
gcc -Iinclude tests/send_icmp.c src/ethernet.c src/arp.c src/arp_table.c src/ipv4.c src/icmp.c src/checksum.c -o send_icmp
```

---

# 實測驗收結果 (Runtime Log)

Terminal A 執行 `sudo ./network`，Terminal B 執行 `sudo ./send_icmp`：

```text
[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : 52:54:00:12:34:56
EtherType   : 0x0800

IPv4 Packet
------------------
Version      : 4
Header Length: 20 bytes
TTL          : 64
Protocol     : 1
Checksum     : 0x66de (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2


ICMP Packet
-------------------
Type       : 8
Code       : 0
Checksum   : 0xf32c (OK)
Identifier : 1234
Sequence   : 1

Frame length: 42 bytes
```

---

# 下一天：Day 9 預告

今天我們成功解析了 ICMP Echo Request 封包。
下一天我們要做第一個真正的網路互動服務 ── **收到 Echo Request 後自動回應 Echo Reply (Ping 答覆)**！
