# Day 12：UDP 與 Port — 封包到底要交給哪個程式？

昨天我們完成了 **IPv4 路由轉發決策（Routing）**：
```text
Ethernet (L2) ──► IPv4 (L3) ──► Routing (Local Delivery vs Forwarding)
```
我們的 Network Stack 已經具備判斷「這個封包是不是要送給我這台主機（`10.0.0.2`）」的能力。

但隨之而來的是傳輸層（Transport Layer, L4）的核心問題：
假設電腦收到 `Destination IP = 10.0.0.2`，這確實是我的主機，但同一台電腦上同時跑著 **Chrome、Discord、DNS Client、線上遊戲**……
**作業系統到底該把這個封包交給哪一個應用程式？**

答案就是：**Port（連接埠）**。

---

# 今日學習目標與成果

- [x] **建立 UDP Header 結構體（`struct udp_hdr`）**：嚴格對齊 RFC 768，透過 `__attribute__((packed))` 確保 Header 剛好為 8 Bytes。
- [x] **實作 IPv4 傳輸層分流（Protocol 17 - UDP）**：在 IPv4 Protocol Dispatcher 中支援 `IPPROTO_UDP`，並呼叫 `udp_receive()`。
- [x] **解析 UDP 欄位**：將網路字節序（Network Byte Order）透過 `ntohs()` 轉為主機字節序，正確印出 Source Port、Destination Port、Length 與 Checksum。
- [x] **建立作業系統級 Socket Table**：
  - `udp_bind()`：模擬應用程式向作業系統註冊/監聽特定 Port。
  - `udp_lookup()`：封包抵達時，快速查詢目標 Port 是否有程式在等待接收。
- [x] **實測驗證（Deliver vs No Listener）**：
  - 目的 Port 8080（有註冊）──► 輸出 `[UDP] Deliver to Port 8080`。
  - 目的 Port 9999（未註冊）──► 輸出 `[UDP] No listener`。
- [x] **實戰剖析 L2 ARP 與 L4 UDP 的連動機制**：釐清發送 UDP 時「先有 ARP 廣播問 MAC，才有 UDP 單播送達」的根本原因。

---

# 核心概念深入剖析

### 1. IP 與 Port 的本質差異：大樓地址 vs 房號

* **IP 位址（L3 網路層）**：負責「**找到哪台主機**」（例如：`10.0.0.2`）。
* **Port 連接埠（L4 傳輸層）**：負責「**找到主機上的哪一個行程/應用程式**」（例如：`10.0.0.2:8080`）。

常見知名 Port（Well-Known Ports, 0 ~ 1023）：
* `53`：DNS（網域名稱解析）
* `80`：HTTP（網頁傳輸）
* `443`：HTTPS（加密網頁傳輸）
* `67 / 68`：DHCP（動態主機設定）

---

### 2. TCP vs UDP 的設計哲學

IPv4 Header 中的 `Protocol` 欄位決定了上層的協定種類：
* `1` ➔ ICMP（網路控制與除錯）
* `6` ➔ TCP（傳輸控制協定）
* `17` ➔ UDP（使用者資料報協定）

| 特性 | TCP (Transmission Control Protocol) | UDP (User Datagram Protocol) |
| :--- | :--- | :--- |
| **連線狀態** | 連線導向（Connection-Oriented，需三向交握） | 無連線（Connectionless） |
| **可靠性** | 保證送達、超時重傳、擁塞控制 | 不保證送達（盡力而為 Best Effort） |
| **封包順序** | 保證順序正確（有 Sequence Number） | 不保證順序（可能亂序） |
| **標頭開銷** | 至少 20 Bytes（複雜） | **固定 8 Bytes（極致輕量）** |
| **適用情境** | 網頁 (HTTP)、檔案傳輸 (FTP)、SSH、資料庫 | DNS、DHCP、語音視訊 (VoIP)、線上遊戲、即時串流 |

---

### 3. Socket Table 的關鍵角色：收與寄的戶籍登記冊

作業系統核心內部的 **Socket Table** 就像大樓管理員的「住戶登記名冊」：

1. **接收封包時（Receive）**：
   - 核心讀取封包的 `Destination Port`。
   - 查詢 Socket Table：
     - 若有程式 `bind(8080)` ➔ 將資料遞交給應用程式緩衝區（**Deliver**）。
     - 若無人登記 ➔ 封包丟棄（**Drop / No listener**）並回覆 ICMP Port Unreachable。
2. **發送封包時（Send）**：
   - 客戶端送出資料時，作業系統會在 Socket Table 挑選一個尚未被使用的臨時埠號（Ephemeral Port，例如 `56994`）作為 `Source Port`，並登記入表。
   - **目的**：當遠端伺服器回覆封包時，回覆的目的地就是這個 `56994`，作業系統才能依照名冊將回信正確交回發送者手中。

---

### 4. UDP Header 結構 (RFC 768)

UDP 標頭極度精簡，僅由 4 個 16-bit 欄位組成，合計剛好 **8 Bytes**：

```text
 0                   15 16                  31
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|            Length             |           Checksum            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                               |
|                    Payload                    |
|                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

* **Source Port（16 bits）**：發送端的應用程式埠號（可選，若不需回覆可填 0）。
* **Destination Port（16 bits）**：接收端的應用程式埠號。
* **Length（16 bits）**：包含 UDP Header + UDP Payload 的總長度（最小值為 8）。
* **Checksum（16 bits）**：涵蓋虛擬標頭（Pseudo Header）、UDP Header 與資料的校驗和。

---

# 關鍵程式碼實作

### 1. `include/udp.h`：結構定義與函式原型

```c
#ifndef UDP_H
#define UDP_H

#include <stdint.h>
#include <stddef.h>

#define UDP_HEADER_LEN 8
#define MAX_UDP_SOCKETS 16

/* UDP Header (RFC 768) - 固定 8 Bytes */
struct udp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed));

/* 簡單的 UDP Socket 結構 */
struct udp_socket
{
    uint16_t port;
};

/* 函式宣告 */
void udp_init(void);
int udp_bind(uint16_t port);
struct udp_socket *udp_lookup(uint16_t port);
void udp_print_header(const struct udp_hdr *udp);
void udp_receive(int fd, const uint8_t *buffer, size_t len);

#endif /* UDP_H */
```

---

### 2. `src/udp.c`：Socket Table 管理與封包接收分派

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ethernet.h"
#include "ipv4.h"
#include "udp.h"

/* Socket Table: 紀錄本機正在監聽哪些 Port */
static struct udp_socket udp_table[MAX_UDP_SOCKETS];

void udp_init(void)
{
    memset(udp_table, 0, sizeof(udp_table));
}

int udp_bind(uint16_t port)
{
    if (port == 0) return -1;
    if (udp_lookup(port) != NULL) return -1; // 已被綁定

    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_table[i].port == 0) {
            udp_table[i].port = port;
            printf("[UDP] Bound to port %u\n", port);
            return 0;
        }
    }
    return -1; // 表格已滿
}

struct udp_socket *udp_lookup(uint16_t port)
{
    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_table[i].port == port) {
            return &udp_table[i];
        }
    }
    return NULL;
}

void udp_print_header(const struct udp_hdr *udp)
{
    printf("\nUDP Packet\n-------------------\n");
    printf("Source Port      : %u\n", ntohs(udp->src_port));
    printf("Destination Port : %u\n", ntohs(udp->dst_port));
    printf("Length           : %u\n", ntohs(udp->length));
    printf("Checksum         : 0x%04x\n\n", ntohs(udp->checksum));
}

void udp_receive(int fd, const uint8_t *frame, size_t len)
{
    (void)fd;

    if (len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr)) return;

    const uint8_t *payload = frame + ETH_HEADER_LEN;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ihl + sizeof(struct udp_hdr)) return;

    const struct udp_hdr *udp = (const struct udp_hdr *)(payload + ihl);

    // 1. 印出 UDP Header
    udp_print_header(udp);

    // 2. 取得目的 Port (需注意網路字節序轉換)
    uint16_t dst_port = ntohs(udp->dst_port);

    // 3. Port Lookup: 查詢 Socket Table
    struct udp_socket *sock = udp_lookup(dst_port);
    if (sock) {
        printf("[UDP] Deliver to Port %u\n", dst_port);
    } else {
        printf("[UDP] No listener\n");
    }
    fflush(stdout);
}
```

---

### 3. `src/tap.c`：IPv4 Protocol Dispatcher 串接

```c
                        // IPv4 Protocol Dispatcher
                        switch (ip->protocol) {
                            case IPPROTO_ICMP:
                                icmp_receive(fd, buffer, n);
                                break;
                            case IPPROTO_UDP:
                                udp_receive(fd, buffer, n);
                                break;
                            default:
                                break;
                        }
```

---

# 實戰驗證與觀察

### 測試步驟

1. **Terminal 1（執行 Network Stack）**：
   ```bash
   make clean && make
   sudo ./network
   ```
   *啟動輸出*：
   ```text
   [UDP] Bound to port 8080
   TAP device: tap0 (UP)
   Waiting for Ethernet frame...
   ```

2. **Terminal 2（Linux 發送測試封包）**：
   ```bash
   # 設定 Linux 主機端 tap0 IP
   sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null || true
   sudo ip link set tap0 up

   # 測試 A：發送到 Port 8080（有監聽）
   echo "hello" | nc -u -w 1 10.0.0.2 8080

   # 測試 B：發送到 Port 9999（無監聽）
   echo "hello" | nc -u -w 1 10.0.0.2 9999
   ```

---

### 關鍵實驗現象分析：為什麼第一次傳送時出現了兩個 Ethernet Frame？

在 Terminal 1 的輸出中，我們目睹了網路底層最真實的運作過程：

```text
Ethernet Frame
-------------------------
Destination : ff:ff:ff:ff:ff:ff
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0806
[ARP] Request for 10.0.0.2 received -> Generating ARP Reply
[ARP] Reply sent (42 bytes)
Frame length: 42 bytes

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
Protocol     : 17
Checksum     : 0x3afb (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] Local Delivery (for me)

UDP Packet
-------------------
Source Port      : 56994
Destination Port : 8080
Length           : 14
Checksum         : 0xa9c0

[UDP] Deliver to Port 8080
Frame length: 48 bytes
```

#### 為什麼有兩個 Frame？
1. **第 1 個 Frame 是 ARP 廣播（問路）**：
   Linux 主機（`10.0.0.1`）想要傳 UDP 給 `10.0.0.2`，但**不知道 `10.0.0.2` 的 MAC 地址**。因此 Linux 必須先發送廣播（`ff:ff:ff:ff:ff:ff`，EtherType `0x0806`）向全區網詢問。我們的 Network Stack 即時回覆了 ARP Reply。
2. **第 2 個 Frame 才是真正的 UDP 單播（送達）**：
   Linux 拿到 MAC 地址後，才真正將 UDP 封包封裝進 Ethernet Frame（Destination `02:00:00:00:00:01`，EtherType `0x0800`，Protocol `17`），順利送達並觸發 `[UDP] Deliver to Port 8080`！
3. **快取效應**：
   若立刻再發送第二次 UDP，由於 Linux 的 ARP Cache 已經記住 MAC，就不會再出現第一個廣播 Frame，直接看到第二個 UDP Frame。

---

# 目前網路堆疊完整架構演進 (Day 12)

```text
                     Network Stack Architecture (Day 12)
 
                              Ethernet (L2)
                                   │
                  ┌────────────────┴────────────────┐
                  │                                 │
             ARP (0x0806)                      IPv4 (0x0800)
                  │                                 │
            ARP Request / Reply               IPv4 Checksum Verify
                  │                                 │
              ARP Table                       TTL Check & Decrement
                                                    │
                                         ┌──────────┴──────────┐
                                         ▼                     ▼
                                    TTL == 0                TTL > 0
                                 (Time Exceeded)               │
                                                               ▼
                                                       Destination Check
                                                               │
                                               ┌───────────────┴───────────────┐
                                               ▼                               ▼
                                         Local Delivery                   Not for me
                                      (ip->dst == LOCAL_IP)                    │
                                               │                               ▼
                                      Protocol Dispatcher                Routing Table
                                               │                        (Lookup Netmask)
                       ┌───────────────────────┴───────────────────────┐       │
                       ▼                                               ▼       ▼
              ICMP (Protocol = 1)                             UDP (Protocol = 17) Route / Forward
                       │                                               │
              ┌────────┴────────┐                              UDP Header Parsing
              │                 │                              (Src/Dst Port, Len)
         Echo Request      Echo Reply                                  │
          (Type = 8)       (Type = 0)                            Socket Table
                                                               (Lookup Dst Port)
                                                                       │
                                                       ┌───────────────┴───────────────┐
                                                       ▼                               ▼
                                                   Port Found                      No Listener
                                              (Deliver to Port)                      (DROP)
```

---

# 下一天：Day 13 預告

今天我們學會了「看懂 UDP」以及作業系統如何透過 Port 找到對應的程式。
明天（Day 13），我們將正式邁入 **L4 應用層資料處理**：
* 讀取 UDP 的 Payload 內容（例如讀到 `"hello"`）。
* 實作真正的 **UDP Echo Server**：把收到的資料打包並透過 TAP 網卡原路發送回去！
