# Day 16：TCP Header 基礎 — 看懂 TCP 封包的骨架

昨天（Day 15）我們完成了第一個應用層協定 DNS Client，整個網路協定路徑為：

```text
Application
    ↓
   DNS
    ↓
   UDP
    ↓
  IPv4
    ↓
Ethernet
```

今天（Day 16），我們正式踏入網際網路最核心、最龐大的傳輸層協定 —— **TCP（Transmission Control Protocol，傳輸控制協定）**！

接下來 Day16～Day21 會形成一個 TCP 小系列：

```text
Day16：讀懂 TCP Header，完成 TCP 封包接收與解析
Day17：建立 TCP Socket Table 與 TCP State Machine
Day18：收到 SYN 後建立連線槽位，並回覆 SYN-ACK
Day19：收到最後 ACK，進入 ESTABLISHED
Day20：在 ESTABLISHED 狀態接收 Payload，並回傳 ACK
Day21：主動送出 Payload，完成最小雙向資料傳輸
```

這個小系列的目標是先做出「最小可觀察、可理解」的 TCP 教學模型，而不是一次補齊真實 Kernel TCP 的所有機制。

現代網際網路幾乎所有重要的應用都建立在 TCP 之上：
```text
HTTP / HTTPS (Web 瀏覽)
SSH (遠端連線)
Git (程式碼版本控制)
MySQL / PostgreSQL (資料庫連線)
SMTP / IMAP (電子郵件傳輸)
```

今天我們在自製網路堆疊中成功打通了 TCP 封包的接收、解析與解構路徑：
```text
Ethernet
    ↓
  IPv4 (Protocol 6)
    ↓
   TCP
```

---

# 今日學習目標與成果

- [x] 理解 TCP 與 UDP 的差異：連線、可靠性、位元組流與標頭大小。
- [x] 定義最小 20 Bytes 的 `struct tcp_hdr`。
- [x] 解析 TCP 主要欄位：Port、SEQ、ACK、Data Offset、Flags、Window、Checksum。
- [x] 認識常見 TCP Flags：SYN、ACK、FIN、RST、PSH、URG。
- [x] 實作 `tcp_receive()`，讓 IPv4 Protocol 6 封包進入 TCP 解析流程。
- [x] 使用 `send_tcp_syn` 測試工具，在 TAP 上送入並解析第一個 TCP SYN 封包。

---

# 核心概念深入剖析

### 1. TCP 與 UDP 的最大差異

| 特性 | UDP (User Datagram Protocol) | TCP (Transmission Control Protocol) |
| :--- | :--- | :--- |
| **連線模式** | 無連線（Connectionless） | 連線導向（Connection-Oriented） |
| **生活比喻** | 普通平信（寄了就走，不理對方是否收到） | 雙向電話 / 掛號信（確保通話建立才講話） |
| **可靠性** | 不保證送達、不保證順序、不自動重傳 | 保證送達、保證順序正確、掉包自動重傳 |
| **傳輸單位** | 獨立封包（Message-based） | 連續位元組串流（Byte Stream） |
| **流量控制** | 無（發送端可隨意狂噴） | 滑動視窗（Sliding Window Flow Control） |
| **擁塞控制** | 無 | 具備慢啟動、擁塞避免機制 |
| **標頭開銷** | 固定 8 Bytes | 最小 20 Bytes（可擴充 Options） |

---

### 2. TCP Header 結構圖解（RFC 793）

標準 TCP 標頭固定最小長度為 **20 Bytes**（若無額外 Option）：

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          Source Port          |       Destination Port        |  (4 Bytes)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Sequence Number                        |  (4 Bytes)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Acknowledgment Number                      |  (4 Bytes)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Data |           |U|A|P|R|S|F|                               |
| Offset| Reserved  |R|C|S|S|Y|I|            Window             |  (4 Bytes)
| (4b)  |   (6b)    |G|K|H|T|N|N|                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|           Checksum            |         Urgent Pointer        |  (4 Bytes)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                    Options and Padding (可選)                 |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

---

### 3. 各欄位職責詳細解密

#### (1) 通訊埠（Source Port & Destination Port）— 各 16 bits (2 Bytes)
- **IP 決定哪台主機，Port 決定哪個程式**。
- `Source Port`：客戶端作業系統動態配發的臨時埠（如 `52144`）。
- `Destination Port`：目的服務埠（如 HTTP `80`、HTTPS `443`、SSH `22`）。
- 讀取時需使用 `ntohs()` 轉換為本機位元組序。

#### (2) 序號（Sequence Number，SEQ）— 32 bits (4 Bytes)
- **TCP 最核心的靈魂欄位**。
- TCP 將傳輸視為無窮無盡的「Byte 串流」。當一段資料被拆成數個封包時，網路環境可能會導致封包順序錯亂或遺失。
- `SEQ` 標明了**「當前這包封包的第一個位元組，在整個資料流中是第幾個 Byte」**。接收端因此能精準拼裝還原原始資料，並發現遺失。
- 在三向交握的第一步（SYN），雙方各自生成隨機的初始序號（ISN, Initial Sequence Number）。
- 讀取時需使用 `ntohl()`。

#### (3) 確認號（Acknowledgment Number，ACK）— 32 bits (4 Bytes)
- **「確認送達」的回執機制**。
- 代表接收端告訴發送端：**「你送來的第 ACK 號之前的資料我都收齊了，下一包請從第 ACK 號開始送！」**
- 只有在 Flags 的 `ACK` bit 為 1 時，此欄位才有效。
- 讀取時需使用 `ntohl()`。

#### (4) 資料位移（Data Offset）— 4 bits
- 標明 **TCP Header 的長度**，單位是 **4 Bytes（32-bit Words）**。
- 最小為 5（`5 × 4 = 20 Bytes`），若帶有 Options 會大於 5。
- 提取方式：`(tcp->data_offset >> 4)`。

#### (5) 滑動視窗（Window Size）— 16 bits (2 Bytes)
- **流量控制（Flow Control）核心**。
- 接收端用來動態通知發送端：**「我的 Buffer 目前還能裝多少 Bytes，請勿傳送超過此上限！」**
- 當接收端消化不及，可宣告 `Window = 0`（Zero Window）迫使發送端暫停，防止接收端緩衝區溢位。

#### (6) 校驗和（Checksum）— 16 bits (2 Bytes)
- TCP Checksum 與 IPv4 Header Checksum 不同：IPv4 Header Checksum **只保護 IPv4 Header 本身**；TCP Checksum 則涵蓋 **TCP Header + TCP Payload**，並額外結合包含來源/目的 IP、Protocol、TCP 長度的「虛擬標頭（Pseudo Header）」進行 16-bit One's Complement 反相校驗。
- 因為加入了 Pseudo Header，TCP 可以檢查封包是否不只資料沒壞，還有沒有被送錯 IP 端點。
- 本系列 Day16～Day21 的 TCP 教學版目前仍暫填 `tcp->checksum = 0`，測試工具也不驗證 TCP Checksum；若要與真實 OS TCP Stack 穩定互通，後續必須實作完整 TCP Pseudo Header Checksum。

#### (7) 緊急指標（Urgent Pointer）— 16 bits (2 Bytes)
- 當 Flags 的 `URG` 設為 1 時生效，標示緊急資料相對於 `SEQ` 的位移量（如傳送 `Ctrl + C` 中斷訊號）。

---

### 4. TCP Flags 控制儀表板

TCP Flags 是單個 Byte 中的位元標誌，每個 bit 代表一個特定的控制訊號：

| Flag | 位元遮罩 (Hex) | 意義與說明 |
| :---: | :---: | :--- |
| **SYN** | `0x02` | **Synchronize**：發起連線建立，同步初始序號（ISN）。 |
| **ACK** | `0x10` | **Acknowledgment**：確認收到資料，ACK 欄位有效。 |
| **FIN** | `0x01` | **Finish**：發送端已無資料要傳送，禮貌性關閉連線。 |
| **RST** | `0x04` | **Reset**：強制重設/中斷連線（如連接未開啟的 Port 或連線崩潰）。 |
| **PSH** | `0x08` | **Push**：請求接收端儘速將緩衝區資料推交應用程式。 |
| **URG** | `0x20` | **Urgent**：緊急指標有效。 |

#### 經典交握範例：
- 連線第一步（客戶端發起）：`Flags: 0x02 [SYN]`
- 連線第二步（伺服端回覆）：`Flags: 0x12 [SYN, ACK]`
- 連線第三步（客戶端確認）：`Flags: 0x10 [ACK]`

---

# 實作架構與核心程式碼

### 1. TCP Header 結構定義 (`include/tcp.h`)

```c
#ifndef TCP_H
#define TCP_H

#include <stdint.h>
#include <stddef.h>

#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

/* TCP Flags 定義 */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP Header 結構 (RFC 793 - 固定最小 20 Bytes) */
struct tcp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;

    uint32_t seq;
    uint32_t ack;

    uint8_t data_offset;
    uint8_t flags;

    uint16_t window;

    uint16_t checksum;

    uint16_t urgent_ptr;
} __attribute__((packed));

void tcp_print_header(const struct tcp_hdr *tcp);
void tcp_receive(int fd, const uint8_t *buffer, size_t len);

#endif /* TCP_H */
```

---

### 2. TCP 接收與格式化解析 (`src/tcp.c`)

```c
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "ethernet.h"
#include "ipv4.h"
#include "tcp.h"

void tcp_print_header(const struct tcp_hdr *tcp)
{
    printf("\nTCP Packet\n");
    printf("----------------------------------------\n");

    printf("Source Port      : %u\n", ntohs(tcp->src_port));
    printf("Destination Port : %u\n", ntohs(tcp->dst_port));

    printf("SEQ              : %u\n", ntohl(tcp->seq));
    printf("ACK              : %u\n", ntohl(tcp->ack));

    printf("Data Offset      : %u (Header Length: %u bytes)\n",
           (tcp->data_offset >> 4),
           (tcp->data_offset >> 4) * 4);

    printf("Flags            : 0x%02x [ %s%s%s%s%s%s]\n",
           tcp->flags,
           (tcp->flags & TCP_SYN) ? "SYN " : "",
           (tcp->flags & TCP_ACK) ? "ACK " : "",
           (tcp->flags & TCP_FIN) ? "FIN " : "",
           (tcp->flags & TCP_RST) ? "RST " : "",
           (tcp->flags & TCP_PSH) ? "PSH " : "",
           (tcp->flags & TCP_URG) ? "URG " : "");

    printf("                   SYN = %d, ACK = %d, FIN = %d, RST = %d\n",
           (tcp->flags & TCP_SYN) ? 1 : 0,
           (tcp->flags & TCP_ACK) ? 1 : 0,
           (tcp->flags & TCP_FIN) ? 1 : 0,
           (tcp->flags & TCP_RST) ? 1 : 0);

    printf("Window           : %u\n", ntohs(tcp->window));
    printf("Checksum         : 0x%04x\n", ntohs(tcp->checksum));
    printf("Urgent Pointer   : %u\n", ntohs(tcp->urgent_ptr));
    printf("----------------------------------------\n");
}

void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    (void)fd;

    // 取得 IPv4 標頭以計算動態 IP Header 長度
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    // 確保封包長度足夠容納 Ethernet + IPv4 + TCP Header
    if (len < ETH_HEADER_LEN + ip_hdr_len + sizeof(struct tcp_hdr)) {
        printf("[TCP] Packet too short\n");
        return;
    }

    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + ip_hdr_len);
    tcp_print_header(tcp);
}
```

---

### 3. IPv4 分流器掛載 TCP (`src/tap.c`)

與 ICMP、UDP 完全對齊的整潔架構：

```c
                        // IPv4 Protocol Dispatcher
                        switch (ip->protocol) {
                            case IPPROTO_ICMP:
                                icmp_receive(fd, buffer, n);
                                break;
                            case IPPROTO_UDP:
                                udp_receive(fd, buffer, n);
                                break;
                            case IPPROTO_TCP:
                                tcp_receive(fd, buffer, n);
                                break;
                            default:
                                break;
                        }
```

---

### 4. 專屬測試工具 (`test/send_tcp_syn.c`)

透過 Linux Raw Socket 注入一包標準的 TCP SYN 封包至 `tap0`：
- `Src Port: 52144`
- `Dst Port: 80`
- `SEQ: 1000`
- `ACK: 0`
- `Flags: TCP_SYN (0x02)`
- `Window: 4096`

---

# 驗收實測結果

### 終端機 1：Protocol Stack (`./network`) 輸出

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
Protocol     : 6
Checksum     : 0x62e5 (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] Local Delivery (for me)

TCP Packet
----------------------------------------
Source Port      : 52144
Destination Port : 80
SEQ              : 1000
ACK              : 0
Data Offset      : 5 (Header Length: 20 bytes)
Flags            : 0x02 [ SYN ]
                   SYN = 1, ACK = 0, FIN = 0, RST = 0
Window           : 4096
Checksum         : 0x0000
Urgent Pointer   : 0
----------------------------------------
Frame length: 54 bytes
```

### 結果分析
1. **L2 Ethernet**：正確接收 EtherType `0x0800` (IPv4)。
2. **L3 IPv4**：正確比對 Destination IP 為本機 `10.0.0.2`，Protocol `6` (TCP)，Checksum 驗證無誤，判定 Local Delivery。
3. **L4 TCP**：成功由 `tcp_receive()` 接收並由 `tcp_print_header()` 完整印出！所有欄位皆精確對應我們在測試工具中設定的數值。

---

# 目前完整的 Protocol Stack 全景

```text
                     Ethernet (L2)
                           │
             ┌─────────────┴─────────────┐
             │                           │
          ARP (0x0806)               IPv4 (0x0800)
             │                           │
        ARP Cache Table     ┌────────────┼────────────┐
                            │            │            │
                        ICMP (1)      UDP (17)      TCP (6)  <-- 【Day 16】
                            │            │            │
                       Echo Reply     DNS / App   Header Parser
                                                      ├── Ports
                                                      ├── Sequence (SEQ)
                                                      ├── Acknowledge (ACK)
                                                      ├── Flags (SYN/ACK...)
                                                      └── Window Size
```

---

# Day 17 預告：TCP Socket Table 與狀態機 (State Machine)

今天我們看懂了 TCP 封包的「骨架」。  
明天，我們將賦予 TCP 真正的「靈魂」：

1. **建立 TCP Socket Table**：紀錄本機監聽哪些 Port（如 `80`、`8080`），以及目前的連線狀態。
2. **TCP 狀態機（State Machine）**：
   - `CLOSED`
   - `LISTEN`
   - `SYN_SENT`
   - `SYN_RECEIVED`
   - `ESTABLISHED`
3. **邁向三向交握（Three-Way Handshake）**：當收到 `SYN` 時，如何變更狀態並準備回覆 `SYN + ACK`！
