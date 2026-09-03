# Day 7：IPv4 Checksum — 驗證封包有沒有壞掉

昨天我們完成了 IPv4 Header 的基本解析。

今天加入 **IPv4 Header Checksum**，讓我們的 Network Stack 可以判斷 IPv4 Header 在傳輸過程中是否被修改。

```text
IPv4 Header
    ↓
Checksum = 0
    ↓
16-bit One's Complement Sum
    ↓
產生 Checksum
    ↓
接收端重新計算
    ↓
0x0000 → OK
其他   → FAIL
```

注意：若只看 One's Complement 加總本身，合法 IPv4 Header 會得到 `0xffff`；本課程的 `ipv4_checksum()` 最後會再做 One's Complement，因此驗證時函式回傳 `0x0000` 代表正確。

---

# 今日學習目標

完成：

* [x] 實作 `ipv4_checksum()`
* [x] 發送 IPv4 封包時計算 Checksum
* [x] 接收端驗證 Checksum
* [x] 測試合法封包
* [x] 故意修改 TTL 驗證錯誤偵測

---

# 1. 核心概念：IPv4 Checksum

IPv4 Header 裡有一個：

```text
Header Checksum
```

它的用途是：

> **偵測 IPv4 Header 是否在傳輸過程中發生變化。**

注意：

```text
IPv4 Checksum
    ↓
只保護 IPv4 Header
```

並不負責驗證 Payload。

---

## 1.1 Checksum 怎麼計算？

IPv4 使用 **16-bit One's Complement Sum**。它的概念可以想成：先把整個 IPv4 Header 切成一段一段 16-bit 數值，全部加總後把超過 16-bit 的 carry 加回低位，最後再取 One's Complement 得到 checksum。(注意，附圖為了方便理解 是使用8-bit一組，實際運算是16-bit一組做計算)

![Day07 IPv4 Header Checksum 運作原理](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day07/Day07_1.png)

這張圖的重點是：Checksum 不是額外保護整個封包的加密機制，而是一個針對 IPv4 Header 的簡單算術檢查。只要 Header 中任何欄位被改變，例如 TTL、Source IP 或 Destination IP，重新計算後的結果就會不同，接收端便能發現 Header 可能已經被修改。

流程可以整理如下：

```text
IPv4 Header
     ↓
切成 16-bit
     ↓
全部相加
     ↓
把 carry 加回來
     ↓
One's Complement
     ↓
Checksum
```

例如：

```text
0x4500
+ 0x003c
+ 0x1234
+ ...
────────
  sum
```

最後：

```c
checksum = ~sum;
```

---

# 2. 建立 Checksum Module

新增：

```text
include/checksum.h
src/checksum.c
```

### `include/checksum.h`

```c
#ifndef CHECKSUM_H
#define CHECKSUM_H

#include <stdint.h>

uint16_t ipv4_checksum(
    const void *data,
    int length
);

#endif
```

### `src/checksum.c`

```c
#include <stdint.h>
#include "checksum.h"

uint16_t ipv4_checksum(
    const void *data,
    int length
)
{
    const uint16_t *ptr = data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    if (length == 1) {
        sum += *((const uint8_t *)ptr);
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}
```

這個版本的 `ipv4_checksum()` 是直接針對封包在記憶體中的 bytes 做 16-bit 加總。

本課程採用以下約定：

* IPv4 Header 中所有多位元組欄位，建立封包時都要先寫成 Network Byte Order。
* `ipv4_checksum()` 的回傳值直接寫回 `ip->checksum`。
* 不要再額外包一層 `htons()`，否則在本實作中可能會讓 checksum 位元組順序顛倒。

---

# 3. 發送端計算 Checksum

計算前一定要先：

```c
ip->checksum = 0;
```

然後：

```c
ip->checksum =
    ipv4_checksum(
        ip,
        sizeof(struct ipv4_hdr)
    );
```

完整流程：

```text
建立 IPv4 Header
       ↓
Checksum = 0
       ↓
ipv4_checksum()
       ↓
得到 0x66e6
       ↓
寫回 Header
       ↓
送出
```

---

# 4. 接收端驗證 Checksum

在 `ipv4.c`：

```c
int ipv4_verify_checksum(
    const struct ipv4_hdr *ip
)
{
    uint8_t ihl =
        ip->version_ihl & 0x0F;

    uint16_t result =
        ipv4_checksum(ip, ihl * 4);

    return result == 0;
}
```

為什麼是 `0`？

因為接收端會把整個 IPv4 Header，包含原本的 Checksum 欄位一起重新計算：

```text
Header
+
原本的 Checksum
        ↓
One's Complement Sum
        ↓
0xffff
        ↓
再做 One's Complement
        ↓
0x0000
```

也就是說：

* 加總結果為 `0xffff` 代表 Header 正確。
* 本課程的 `ipv4_checksum()` 會回傳 `~sum`，所以最後得到 `0x0000`。

因此 `result == 0` 代表沒有發現錯誤。

---

# 5. 印出驗證結果

修改 `ipv4_print_header()`：

```c
void ipv4_print_header(
    const struct ipv4_hdr *ip
)
{
    uint8_t version =
        ip->version_ihl >> 4;

    uint8_t ihl =
        ip->version_ihl & 0x0F;

    int is_correct =
        ipv4_verify_checksum(ip);

    printf("\nIPv4 Packet\n");
    printf("------------------\n");

    printf("Version      : %u\n", version);
    printf("Header Length: %u bytes\n", ihl * 4);
    printf("TTL          : %u\n", ip->ttl);
    printf("Protocol     : %u\n", ip->protocol);

    printf(
        "Checksum     : 0x%04x (%s)\n",
        ntohs(ip->checksum),
        is_correct ? "OK" : "FAIL"
    );

    printf("Source IP    : ");
    print_ip(ip->src_ip);

    printf("\nDestination IP : ");
    print_ip(ip->dst_ip);

    printf("\n\n");
}
```

---

# 6. 實戰測試

## 測試 1：合法封包

建立：

```c
ip->ttl = 64;

ip->checksum = 0;

ip->checksum =
    ipv4_checksum(
        ip,
        sizeof(struct ipv4_hdr)
    );
```

執行後：

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
Checksum     : 0x66e6 (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

Frame length: 34 bytes
```

---

## 測試 2：故意破壞封包

先計算：

```c
ip->ttl = 64;
ip->checksum = 0;

ip->checksum =
    ipv4_checksum(
        ip,
        sizeof(struct ipv4_hdr)
    );
```

然後**故意修改 TTL，但不重新計算 Checksum**：

```c
ip->ttl = 63;
```

結果：

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
TTL          : 63
Protocol     : 1
Checksum     : 0x66e6 (FAIL)

Frame length: 34 bytes
```

成功偵測到：

```text
TTL 被修改
   ↓
Checksum 不匹配
   ↓
FAIL
```

---

# 7. 今日 Network Stack

目前我們已經完成：

```text
Ethernet
   │
   ├── ARP
   │     └── ARP Table
   │
   └── IPv4
         ├── Header Parsing
         └── Header Checksum
```

也就是：

```text
Ethernet Frame
      ↓
Ethernet Filter
      ↓
EtherType
      ↓
IPv4
      ↓
IPv4 Header
      ↓
Checksum Verification
      ↓
   OK / FAIL
```

---

# 常見問題（Q&A）

### Q1：Checksum 是加密嗎？

不是。

它主要用來**偵測意外的資料錯誤**，不是安全機制。

### Q2：為什麼只驗證 Header？

因為 IPv4 Header Checksum 的規範只涵蓋 IPv4 Header，Payload 則由 TCP、UDP 等協定處理。

### Q3：為什麼計算前要設成 0？

因為 Checksum 本身也是 Header 的一部分：

```c
ip->checksum = 0;
```

先排除原本的 Checksum，才能計算新的值。

---

# 今日總結

今天我們完成了 IPv4 Checksum：

```text
發送端：

Header
 ↓
Checksum = 0
 ↓
計算
 ↓
Checksum = 0x66e6
```

接收端：

```text
完整 Header
 ↓
重新計算
 ↓
0x0000 → OK
其他   → FAIL
```

並成功實驗：

```text
TTL = 64 → Checksum OK
TTL = 63 → Checksum FAIL
```

我們的 IPv4 實作現在已經不只是「解析封包」，而是能夠**驗證封包完整性**。

---

# 下一天：Day 8

明天進入 **ICMP**：

```text
Ethernet
    ↓
IPv4
    ↓
Protocol = 1
    ↓
ICMP
    ↓
Echo Request
    ↓
Echo Reply
```

最終目標就是讓我們自己寫的 Network Stack 能夠真正回答：

```bash
ping 10.0.0.2
```

也就是開始實作我們自己的 **Ping**。
