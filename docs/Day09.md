# Day 9：ICMP Echo Reply — 讓你的網路堆疊真正「回應 Ping」

昨天我們完成了 ICMP Echo Request 的接收與解析。
今天我們不再只是「被動解析封包」，而是讓我們的網路堆疊具備真正的**雙向互動能力** ── **自動回應 Ping (ICMP Echo Reply)**！

搭配在 Day 5 健全的 **自動 ARP Reply** 機制，我們的網路堆疊正式達成 **「隨插即用（Plug & Play）」** 的全自動雙向互動！

```text
Host (10.0.0.1)
  │
  ├─ 1. ARP Request ("Who has 10.0.0.2?") ──────► 你的 Network Stack (10.0.0.2)
  │                                                      │
  │ ◄─ 2. ARP Reply ("10.0.0.2 is at LOCAL_MAC") ────────┘ (自動回覆 MAC)
  │
  ├─ 3. ICMP Echo Request (Type = 8) ────────────► 你的 Network Stack (10.0.0.2)
  │                                                      │
  │ ◄─ 4. ICMP Echo Reply (Type = 0) ─────────────┘ (自動回覆 Ping)
  ▼
Host 收到 64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=...
```

![Day09 ICMP Echo Request / Echo Reply 往返流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day09/Day09_1.png)

---

# 今日學習目標與成果

完成以下功能：
- [x] 建立 `icmp_handle()` 函式，辨識 Echo Request 並轉換為 Echo Reply
- [x] 保留 ICMP Identifier、Sequence 與 Payload 原封不動
- [x] 重新計算包含完整 Payload 的 ICMP Checksum
- [x] 在 Ethernet 層交換來源與目的 MAC 地址 (`src_mac <-> dst_mac`)
- [x] 在 IPv4 層交換來源與目的 IP 地址 (`src_ip <-> dst_ip`)，重設 TTL = 64 並重新計算 IPv4 Checksum
- [x] 透過 TAP 虛擬網卡檔案描述符呼叫 `write(fd, reply, n)` 將 Frame 發送回 Linux 核心
- [x] 與 ARP Reply 協同工作：免手動設定靜態 ARP 表，Host 執行 `ping 10.0.0.2` 全自動秒通！

---

# 核心概念與協定反轉機制

Ping 的回應流程涉及跨層級的「位址與型態對調」：

### 1. 各層反轉職責

| 層級 (Layer) | 收到 Request | 修改為 Reply | 額外動作 |
| :--- | :--- | :--- | :--- |
| **L2 (Ethernet)** | `SRC = Host MAC`<br>`DST = Our MAC` | `SRC = Our MAC`<br>`DST = Host MAC` | 交換 MAC 地址 |
| **L3 (IPv4)** | `SRC = 10.0.0.1`<br>`DST = 10.0.0.2` | `SRC = 10.0.0.2`<br>`DST = 10.0.0.1` | 重設 `TTL = 64`，重新計算 IPv4 Checksum |
| **L4 (ICMP)** | `Type = 8` (Echo Request)<br>`Code = 0` | `Type = 0` (Echo Reply)<br>`Code = 0` | 保留 ID/Seq/Payload，重新計算 ICMP Checksum |

```text
                   RX Frame (Request)
                          │
                          ▼
            ┌───────────────────────────┐
            │  Ethernet: Swap MAC       │
            │  IPv4:     Swap IP, TTL   │
            │  ICMP:     Type 8 -> 0    │
            └─────────────┬─────────────┘
                          ▼
            ┌───────────────────────────┐
            │  Recalculate IPv4 Checksum│
            │  Recalculate ICMP Checksum│
            └─────────────┬─────────────┘
                          ▼
                   TX Frame (Reply)
```

![Day09 Echo Request 轉 Echo Reply 欄位反轉圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day09/Day09_2.png)

### 2. 為什麼 Checksum 必須包含 Payload？
* Linux 標準 `ping` 預設會附帶 **56 bytes 的隨機 Payload**。
* ICMP 的校驗和演算法涵蓋整個 ICMP 訊息區段（**ICMP Header 8 bytes + 後方所有的 Payload**）。
* 計算時必須將 `icmp->checksum` 先歸零，再以實際 ICMP 長度進行 16-bit 一補數計算。

### 3. TAP 雙向架構與系統呼叫
* **接收 (RX)**：`read(fd, buffer, sizeof(buffer))` 從虛擬網卡讀取由 Host 端送進來的原始封包。
* **發送 (TX)**：`write(fd, reply, n)` 透過標準 POSIX 系統呼叫將組裝好的完整 Frame 送回虛擬網卡，交由 Linux 核心接收處理。

---

# 今日新增與修改程式碼

### 1. `include/icmp.h`

```c
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
void icmp_handle(uint8_t *icmp_payload, size_t icmp_len);

#endif
```

---

### 2. `src/icmp.c`

```c
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

void icmp_handle(uint8_t *packet, size_t length)
{
    if (length < sizeof(struct icmp_hdr))
        return;

    struct icmp_hdr *icmp = (struct icmp_hdr *)packet;

    // 只處理 Echo Request (Type = 8)
    if (icmp->type != ICMP_ECHO_REQUEST) {
        return;
    }

    printf("[ICMP] Echo Request received -> Generating Echo Reply\n");

    // 1. 修改 Type 為 Echo Reply (0)，Code 保持 0
    icmp->type = ICMP_ECHO_REPLY;

    // 2. 重新計算 ICMP Checksum (包含 Header + 所有 Payload)
    icmp->checksum = 0;
    icmp->checksum = ipv4_checksum(packet, length);
}
```

---

### 3. `src/tap.c` (主迴圈整合交換與回送)

```c
                        case IPPROTO_ICMP:
                            if (payload_len >= sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)) {
                                const struct icmp_hdr *icmp_in =
                                    (const struct icmp_hdr *)(payload + sizeof(struct ipv4_hdr));
                                size_t icmp_in_len = payload_len - sizeof(struct ipv4_hdr);

                                icmp_print_header(icmp_in, icmp_in_len);

                                // 如果是 Echo Request，建立並送出 Reply
                                if (icmp_in->type == ICMP_ECHO_REQUEST) {
                                    uint8_t reply[2048];
                                    memcpy(reply, buffer, n); // 複製收到的完整 Frame

                                    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
                                    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
                                    uint8_t *icmp_out = reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr);
                                    size_t icmp_len = n - (ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

                                    // 1. 交換 Ethernet MAC
                                    uint8_t temp_mac[ETH_ADDR_LEN];
                                    memcpy(temp_mac, eth_out->dst, ETH_ADDR_LEN);
                                    memcpy(eth_out->dst, eth_out->src, ETH_ADDR_LEN);
                                    memcpy(eth_out->src, temp_mac, ETH_ADDR_LEN);

                                    // 2. 交換 IPv4 來源與目的 IP，並更新 TTL 與 Checksum
                                    uint32_t temp_ip = ip_out->dst_ip;
                                    ip_out->dst_ip = ip_out->src_ip;
                                    ip_out->src_ip = temp_ip;
                                    ip_out->ttl = 64;
                                    ip_out->checksum = 0;
                                    ip_out->checksum = ipv4_checksum(ip_out, sizeof(struct ipv4_hdr));

                                    // 3. 修改 ICMP 為 Echo Reply 並重算 ICMP Checksum
                                    icmp_handle(icmp_out, icmp_len);

                                    // 4. 送出 Reply Frame 回 tap 介面 (TX)
                                    ssize_t sent = write(fd, reply, n);
                                    if (sent < 0) {
                                        perror("[ICMP] write to tap failed");
                                    } else {
                                        printf("[ICMP] Echo Reply sent (%ld bytes)\n", sent);
                                    }
                                }
                            }
                            break;
```

---

# 編譯與測試驗證

### 1. 編譯網路堆疊

```bash
gcc -Iinclude src/tap.c src/ethernet.c src/arp.c src/arp_table.c src/ipv4.c src/icmp.c src/checksum.c -o network
```

### 2. 啟動網路堆疊 (Terminal 1)

```bash
sudo ./network
```

### 3. 設定 Host 網路並發送 Ping (Terminal 2)

```bash
# 1. 為 Host 端的 tap0 指派 IP (若尚未指派)
sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null || true

# 2. 直接發送真實 Ping 測試（網路堆疊會自動回應 ARP 與 ICMP Reply，免手動設定 ARP！）
ping -c 4 10.0.0.2
```

---

# 實測驗收結果 (Runtime Log)

### Terminal 1 (`./network`) 輸出：

```text
[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0806

[ARP] Request for 10.0.0.2 received -> Generating ARP Reply
[ARP] Reply sent (42 bytes)

[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0800

IPv4 Packet
------------------
Version      : 4
Header Length: 20 bytes
TTL          : 64
Protocol     : 1
Checksum     : 0xfb02 (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

ICMP Packet
-------------------
Type       : 8
Code       : 0
Checksum   : 0x942d (OK)
Identifier : 15649
Sequence   : 1

[ICMP] Echo Request received -> Generating Echo Reply
[ICMP] Echo Reply sent (98 bytes)
Frame length: 98 bytes
```

### Terminal 2 (`ping`) 輸出：

```text
PING 10.0.0.2 (10.0.0.2) 56(84) bytes of data.
64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=0.421 ms
64 bytes from 10.0.0.2: icmp_seq=2 ttl=64 time=0.385 ms
64 bytes from 10.0.0.2: icmp_seq=3 ttl=64 time=0.392 ms
64 bytes from 10.0.0.2: icmp_seq=4 ttl=64 time=0.401 ms

--- 10.0.0.2 ping statistics ---
4 packets transmitted, 4 received, 0% packet loss, time 3004ms
rtt min/avg/max/mdev = 0.385/0.399/0.421/0.014 ms
```

🎉 **我們親手打造的 Network Stack 第一次具備了與真實作業系統全自動、雙向通訊的完整能力！**

---

# 目前網路堆疊完整架構

```text
                    Network Stack Architecture

                             Ethernet (L2)
                                  │
                 ┌────────────────┴────────────────┐
                 │                                 │
                ARP (0x0806)                      IPv4 (0x0800)
                 │                                 │
           ARP Request / Reply               IPv4 Header Checksum
                 │                                 │
             ARP Table                            ICMP (Protocol = 1)
                                                   │
                                          ┌────────┴────────┐
                                          │                 │
                                     Echo Request      Echo Reply
                                      (Type = 8)       (Type = 0)
```

---

# 下一天：Day 10 預告

在 Day 9 我們扮演的是**網路終端設備 (Endpoint)**。
下一天我們將邁入 Router 的核心領域 ── **TTL 機制與 ICMP Time Exceeded (Type = 11)**！

當封包每經過一個路由器，TTL 就會減 1；當 `TTL == 0` 時，路由器必須丟棄封包並回傳 Time Exceeded。
這正是 `traceroute` 工具能探測網路路徑的關鍵原理。
