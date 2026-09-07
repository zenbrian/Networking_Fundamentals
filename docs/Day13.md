# Day 13：UDP 資料收發 — 第一次收到真正的應用層資料

昨天（Day 12）我們完成了 **UDP Header 解析與 Port Lookup**：
```text
Ethernet (L2) ──► IPv4 (L3) ──► UDP (L4) ──► Port Lookup
```
我們的 Network Stack 已經能夠辨識出「這個 UDP 封包是要交給哪個 Port（例如 8080）」。

然而昨天我們只解析了 8 Bytes 的 UDP 標頭就停下了腳步，封包內包覆的**真正資料（Payload）**完全沒有被提取與呈現。

今天（Day 13），我們正式打通了作業系統核心與使用者空間應用程式的最後一哩路：
```text
Ethernet (L2) ──► IPv4 (L3) ──► UDP (L4) ──► Payload 提取 ──► Application Callback
```

當外部客戶端送出字串資料時，我們的網路堆疊成功將資料遞交給註冊在 8080 Port 的應用程式，在螢幕上印出：
```text
=== UDP APP ===
hello world
===============
```

---

# 今日學習目標與成果

- [x] **掌握 UDP 長度計算規範（RFC 768）**：釐清 UDP Header 中的 `length` 包含 **8 Bytes Header + Payload**，並透過 `ntohs()` 轉換計算出精確的 `payload_len`。
- [x] **實作 Payload 記憶體定位**：透過指標運算 `(uint8_t *)udp + sizeof(struct udp_hdr)` 準確定位應用層資料的記憶體起始位址。
- [x] **設計核心至應用層的 Callback 機制**：
  - 定義 `typedef void (*udp_handler_t)(const uint8_t *data, size_t len);`
  - 升級 `struct udp_socket` 與 `udp_bind()`，實現 Kernel 與 Application 的架構解耦。
- [x] **實作第一個 UDP 應用程式（`udp_echo_app`）**：模擬真實 Daemon 服務，在主程式啟動時向核心註冊監聽 Port 8080。
- [x] **完整鏈路實測與位元組級驗算（Byte-level Verification）**：
  - 發送 `hello world\n`（12 bytes）。
  - 精確驗證總封包長度 54 Bytes = Ethernet (14) + IPv4 (20) + UDP Header (8) + Payload (12)。

---

# 核心概念深入剖析

### 1. UDP 封包記憶體佈局與長度計算

UDP 的資料單元（Datagram）由兩部分組成：
```text
+-----------------------+-----------------------------+
|  UDP Header (8 Bytes) |   Payload (應用層真實資料)    |
+-----------------------+-----------------------------+
▲                       ▲
│                       │
udp                     udp + sizeof(struct udp_hdr)
```

在 UDP 標頭中，`length` 欄位定義如下：
* **包含範圍**：UDP Header（8 Bytes）+ Payload 的總長度。
* **最小值**：`8`（代表 Payload 為 0，純空封包）。

因此計算 Payload 長度的公式為：
```c
uint16_t udp_len = ntohs(udp->length);

// 防禦性檢查：長度不能小於標頭本身
if (udp_len < sizeof(struct udp_hdr)) {
    return;
}

size_t payload_len = udp_len - sizeof(struct udp_hdr);
const uint8_t *udp_payload = (const uint8_t *)udp + sizeof(struct udp_hdr);
```

---

### 2. 核心（Kernel）與應用程式（Application）的邊界：為什麼需要 Callback？

在現代作業系統中，網路協定堆疊（Kernel Space）與應用程式（User Space）必須嚴格職責分離：

```text
+-------------------------------------------------------------+
|                    Application Layer                        |
|  [udp_echo_app]     [DNS Server (53)]   [Game Server (9000)]|
+-------------------------------------------------------------+
                              ▲
                       Callback 回呼
                              │
+-------------------------------------------------------------+
|                    Transport Layer (UDP)                    |
|       Socket Table: Port 8080 ──► Handler 指標               |
+-------------------------------------------------------------+
|                    Network Layer (IPv4)                     |
+-------------------------------------------------------------+
|                    Data Link Layer (Ethernet)               |
+-------------------------------------------------------------+
```

#### 為什麼不能直接在 `udp_receive()` 裡處理業務邏輯？
* **高內聚、低耦合**：網路堆疊的核心職責是封包檢查、校驗和、路由與傳輸分流。它不應該也不可能知道「8080 是文字 echo」、「53 是網域名稱查詢」還是「遊戲角色移動」。
* **函式指標（Function Pointer / Callback）的精妙之處**：
  核心只負責維護一張 Socket Table：
  ```c
  struct udp_socket {
      uint16_t port;
      udp_handler_t handler; // 登記該 Port 的聯絡窗口
  };
  ```
  當應用程式呼叫 `udp_bind(8080, udp_echo_app)`，就是告訴核心：「如果有送往 8080 的封包，請回呼我的 `udp_echo_app` 函式！」

#### 概念類比：Web API 路由 vs Transport Layer Port
| Web API 路由（High-Level） | 網路傳輸層（Low-Level） |
| :--- | :--- |
| **Route Path**（例如 `/api/v1/echo`） | **Port 號碼**（例如 `8080`） |
| **Request Body**（JSON / Text） | **UDP Payload**（原始位元組資料） |
| **Controller / Handler 函式** | **`udp_handler_t` Callback 函式** |

---

# 關鍵程式碼實作

### 1. `include/udp.h`：Callback 型態與 Socket 結構升級

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

/* 應用程式回呼函式型態：接收資料指標與資料長度 */
typedef void (*udp_handler_t)(const uint8_t *data, size_t len);

/* UDP Socket 結構 */
struct udp_socket
{
    uint16_t port;         // 監聽的 Port
    udp_handler_t handler; // 收到資料時要執行的 Callback 函式
};

/* 函式宣告 */
void udp_init(void);
int udp_bind(uint16_t port, udp_handler_t handler);
struct udp_socket *udp_lookup(uint16_t port);
void udp_print_header(const struct udp_hdr *udp);
void udp_dump_payload(const uint8_t *payload, size_t len);
void udp_receive(int fd, const uint8_t *buffer, size_t len);

#endif /* UDP_H */
```

---

### 2. `src/udp.c`：Payload 解析與應用程式交付

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "ethernet.h"
#include "ipv4.h"
#include "udp.h"

static struct udp_socket udp_table[MAX_UDP_SOCKETS];

void udp_init(void)
{
    memset(udp_table, 0, sizeof(udp_table));
}

int udp_bind(uint16_t port, udp_handler_t handler)
{
    if (port == 0) return -1;
    if (udp_lookup(port) != NULL) return -1;

    for (int i = 0; i < MAX_UDP_SOCKETS; i++) {
        if (udp_table[i].port == 0) {
            udp_table[i].port = port;
            udp_table[i].handler = handler;
            printf("[UDP] Bound to port %u\n", port);
            return 0;
        }
    }
    return -1;
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

void udp_dump_payload(const uint8_t *payload, size_t len)
{
    printf("Payload (%zu bytes):\n", len);
    for (size_t i = 0; i < len; i++) {
        putchar(payload[i]);
    }
    printf("\n");
}

void udp_receive(int fd, const uint8_t *frame, size_t len)
{
    (void)fd;

    if (len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct udp_hdr)) {
        return;
    }

    const uint8_t *payload = frame + ETH_HEADER_LEN;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ihl + sizeof(struct udp_hdr)) {
        return;
    }

    const struct udp_hdr *udp = (const struct udp_hdr *)(payload + ihl);

    // 1. 印出 UDP Header
    udp_print_header(udp);

    // 2. 取得目的 Port 與查表
    uint16_t dst_port = ntohs(udp->dst_port);
    struct udp_socket *sock = udp_lookup(dst_port);

    // 3. 提取 Payload 與計算長度
    uint16_t udp_len = ntohs(udp->length);
    if (udp_len < sizeof(struct udp_hdr)) {
        return;
    }
    size_t payload_len = udp_len - sizeof(struct udp_hdr);
    const uint8_t *udp_payload = (const uint8_t *)udp + sizeof(struct udp_hdr);

    // 4. 交付至應用程式（Deliver）
    if (sock) {
        printf("[UDP] Deliver to Port %u\n", dst_port);
        udp_dump_payload(udp_payload, payload_len);

        if (sock->handler) {
            sock->handler(udp_payload, payload_len);
        }
    } else {
        printf("[UDP] No listener\n");
    }
    fflush(stdout);
}
```

---

### 3. `src/tap.c`：應用程式定義與註冊

```c
/* 第一個 UDP 應用程式：收到什麼就印出什麼 */
void udp_echo_app(const uint8_t *data, size_t len)
{
    printf("\n=== UDP APP ===\n");
    // fwrite 可以精確輸出 len 長度，不怕資料中包含 '\0'
    fwrite(data, 1, len, stdout);
    printf("===============\n\n");
    fflush(stdout);
}

int main()
{
    ...
    udp_init();
    udp_bind(8080, udp_echo_app); // 將 8080 Port 綁定給 echo app
    ...
}
```

---

# 實戰驗收與封包剖析

### 測試指令

#### Terminal 1（Network Stack）：
```bash
make network
sudo ./network
```

#### Terminal 2（Linux Client）：
```bash
sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null || true
sudo ip link set tap0 up

# 送出測試字串
echo "hello world" | nc -u -w 1 10.0.0.2 8080
```

---

### 終端機真實輸出結果

```text
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
Checksum     : 0x6797 (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] Local Delivery (for me)

UDP Packet
-------------------
Source Port      : 49000
Destination Port : 8080
Length           : 20
Checksum         : 0x7af2

[UDP] Deliver to Port 8080
Payload (12 bytes):
hello world


=== UDP APP ===
hello world
===============

Frame length: 54 bytes
```

---

### 數據位元組精準驗算（Byte-by-Byte Breakdown）

| 協定層級 | 欄位 / 內容 | 長度 | 累計長度 | 說明 |
| :--- | :--- | :--- | :--- | :--- |
| **L2 Ethernet** | MAC Header | 14 Bytes | 14 Bytes | 目的 MAC (02:00:00:00:00:01) + 來源 MAC + EtherType 0x0800 |
| **L3 IPv4** | IP Header | 20 Bytes | 34 Bytes | IHL=5 (20 bytes), Protocol=17 (UDP), 10.0.0.1 ➔ 10.0.0.2 |
| **L4 UDP** | UDP Header | 8 Bytes | 42 Bytes | Src=49000, Dst=8080, **Length=20** |
| **L7 Application** | Payload 資料 | 12 Bytes | **54 Bytes** | 字串 `"hello world\n"`（11 個字母 + 1 個換行字元） |

* **UDP Length** = $8 \text{ (UDP Header)} + 12 \text{ (Payload)} = 20 \text{ Bytes}$
* **總 Frame Length** = $14 + 20 + 8 + 12 = 54 \text{ Bytes}$

每一個數值在記憶體與日誌中均獲得 100% 精準吻合！

---

# 思考與總結：從 UDP 到 TCP 的分水嶺

到今天為止，我們完成的網路堆疊已經具備完整的向下與向上傳輸鏈路：

```text
+------------------------------------+
|            Application             |  <-- udp_echo_app (收到了 "hello world")
+------------------------------------+
                  ▲
+------------------------------------+
|             UDP Socket             |  <-- udp_table (8080 -> handler)
+------------------------------------+
                  ▲
+------------------------------------+
|                UDP                 |  <-- 解析 Port、驗證 Length、提取 Payload
+------------------------------------+
                  ▲
+------------------------------------+
|                IPv4                |  <-- 路由轉發與本機交付決策 (Local Delivery)
+------------------------------------+
                  ▲
+------------------------------------+
|                ARP                 |  <-- IP 與 MAC 映射快取
+------------------------------------+
                  ▲
+------------------------------------+
|              Ethernet              |  <-- 訊框篩選與 EtherType 分流
+------------------------------------+
                  ▲
+------------------------------------+
|             TAP Device             |  <-- 虛擬網路介面 (tap0)
+------------------------------------+
```

### UDP 的特性回顧：
* **收到就交付**：沒有連線建立（Handshake）、沒有確認送達（ACK）、沒有重傳（Retransmission）。
* **極速簡潔**：非常適合 DNS、VoIP、即時遊戲串流等重視低延遲的場景。

---

# Day 14 預告：封包組裝與主動發送 — UDP Sender

完成了接收路徑（Receive Path）後，明天我們將打造逆向的傳送路徑（Transmit Path）：
```text
Application ──► UDP ──► IPv4 ──► ARP Lookup ──► Ethernet ──► TAP
```
你將會親手實作：
* **由上至下的封包封裝（Encapsulation）**
* **UDP Header 與 Payload 組裝**
* **IPv4 標頭封裝與 Checksum 計算**
* **透過 ARP Table 查詢目的 MAC 位址並封裝 Ethernet Frame**
* **使用 `write()` 透過 TAP 設備送出真實 UDP 封包**
* 透過 `nc -lu 9999` 成功接收來自你的自製 Network Stack 發出的問候！
