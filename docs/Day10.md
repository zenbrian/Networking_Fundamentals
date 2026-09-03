# Day 10：TTL 與 ICMP Time Exceeded — Router 如何防止封包永遠繞圈與 traceroute 的祕密

昨天我們完成了 ICMP Echo Reply，讓我們的 Network Stack 具備了主機端（Endpoint）回應 Ping 的能力。

今天我們要邁向路由器（Router）的核心思維：**生命週期限制（Time-to-Live, TTL）** 與 **ICMP Time Exceeded (Type = 11)**！
同時，我們將深入剖析網路工程師每日必用的 `traceroute` 診斷工具背後的真正原理。

```text
IPv4 Packet (Inbound)
          │
          ▼
      TTL - 1
          │
    ┌─────┴─────┐
    ▼           ▼
 TTL > 0     TTL == 0
 (正常轉送)  (壽命耗盡)
    │           │
    ▼           ▼
 Forward      DROP (丟棄封包)
                │
                ▼
      ICMP Time Exceeded (Type = 11, Code = 0)
      附帶「原 IP 標頭 + 前 8 Bytes 原資料」回覆來源端
```

---

# 今日學習目標與成果

完成以下功能與架構演進：
- [x] **理解 TTL 核心防護價值**：掌握 Routing Loop（路由迴圈）產生的原因與 TTL 防止封包在網路世界無限繞圈的自毀機制。
- [x] **實作 `ipv4_decrement_ttl()`**：模擬路由器跳轉（Hop-by-Hop）行為，對進來的 IPv4 封包執行 TTL 扣減與過期檢查。
- [x] **支援 ICMP Type 11 (Time Exceeded)**：遵循 RFC 792 規範定義常數並實作錯誤訊息建置。
- [x] **封裝受害封包資訊**：在 ICMP 錯誤回覆中完整附帶「原始 IPv4 標頭 + 原始 Payload 前 8 bytes」，讓來源端能夠識別是哪一個通訊流或封包發生異常。
- [x] **解構 `traceroute` 運作原理**：徹底理解 traceroute 如何透過故意遞增 TTL（TTL=1, 2, 3...）迫使沿途路由器依序報錯，進而繪製出端到端路徑。
- [x] **網路棧模組化架構重構（Refactoring）**：將原本暫存於 `tap.c` 的 Echo Reply 邏輯與 Time Exceeded 一同封裝進 `src/icmp.c`（實作 `icmp_receive()` 與 `icmp_send_time_exceeded()`），使 `tap.c` 回歸純粹的網卡驅動與 EtherType 分流器。

---

# 核心概念深入剖析

### 1. 為什麼需要 TTL？
在複雜的互聯網拓撲中，若兩台路由器之間因為靜態路由設定錯誤或動態路由協定收斂延遲，形成互指迴圈：

```text
         ┌─────────────┐
         │  Router A   │◄────────────┐
         └──────┬──────┘             │
                │                    │
                ▼                    │
         ┌─────────────┐             │ (Looping Forever!)
         │  Router B   │─────────────┘
         └─────────────┘
```

* **若沒有 TTL**：這個封包將會在這兩個節點之間無限狂飆，耗盡線路頻寬與 CPU 資源，甚至造成廣播風暴與網路癱瘓。
* **有 TTL（預設如 64 或 128）**：封包每經過一台 Router，TTL 減 1。經過 64 跳後歸零，封包自然死亡被 Drop，終止迴圈。

---

### 2. 真正的 Router 行為：不只丟棄，還要報錯

當 Router 收到 `TTL == 1` 的封包時：
1. `TTL--` 變為 `0`。
2. Router 丟棄該封包。
3. Router 主動組裝一個 **ICMP Time Exceeded (Type = 11, Code = 0)** 封包發回給來源端（告訴來源端：「你的封包死在我這裡了」）。

```text
發送端 (10.0.0.1)                                  我們的 Router (10.0.0.2)
       │                                                      │
       ├──── 1. 送出封包 (TTL = 1) ──────────────────────────►│
       │                                                      │ (TTL-- 變成 0 -> DROP!)
       │                                                      │
       │◄─── 2. ICMP Time Exceeded (Type 11, Code 0) ─────────┤
       ▼                                                      ▼
收到警告訊息：
"From 10.0.0.2: Time to live exceeded"
```

---

### 3. RFC 792 規範：為什麼 ICMP 錯誤訊息要附帶原封包？

假設發送端主機同時在做很多事情：
* 背景在下載檔案（TCP）
* 正在解析網域名稱（DNS 53 UDP）
* 正好送出一個 ICMP Ping

當發送端收到一個 ICMP Time Exceeded 時，它怎麼知道是哪一個程式送的封包死掉了？

**答案是：RFC 792 規定，所有 ICMP 差錯報文（Error Message）必須附帶：**
```text
┌─────────────────┬──────────────────────────────────────────────┐
│ ICMP Header     │ Type=11, Code=0, Checksum, Unused (4 bytes)  │
├─────────────────┼──────────────────────────────────────────────┤
│                 │ 原始封包的 IPv4 Header (20 bytes)            │
│ ICMP Payload    │ ＋                                           │
│                 │ 原始封包 Payload 的前 8 bytes (如 TCP/UDP 埠號│
│                 │ 或 ICMP ID/Seq)                              │
└─────────────────┴──────────────────────────────────────────────┘
```
因為 TCP 和 UDP 的 Source Port / Destination Port 正好位在 Payload 的**前 4 個 bytes**，ICMP Echo 的 ID 和 Seq 也位在**前 4 個 bytes**。
附帶這 8 個 bytes，發送端核心就能透過來源與目的 Port 精準找到到底是哪一個 Socket、哪一個 Process 的封包逾期！

---

### 4. `traceroute` 的本質

`traceroute` 根本不需要路由器有任何特異功能，它僅僅是利用了 TTL 扣減的標準行為：

```text
第 1 次探測：送出 TTL = 1 ──► 第 1 台 Router (TTL變0) ──► 回覆 Type 11 (得到第 1 跳 IP)
第 2 次探測：送出 TTL = 2 ──► 通過第 1 台 (變1) ──► 第 2 台 Router (TTL變0) ──► 回覆 Type 11 (得到第 2 跳 IP)
第 3 次探測：送出 TTL = 3 ──► 通過第 1, 2 台 ──► 第 3 台 Router (TTL變0) ──► 回覆 Type 11 (得到第 3 跳 IP)
...
直到抵達最終目的地，目的端回覆 ICMP Echo Reply (或 Port Unreachable)，探測結束！
```

---

# 今日程式碼改動詳解

### 1. `include/ipv4.h` 與 `src/ipv4.c`：實作 TTL 遞減檢查

在 [include/ipv4.h](file:///c:/Users/zenboen/Tutorial/Networking_Fundamentals/include/ipv4.h) 宣告：
```c
int ipv4_decrement_ttl(struct ipv4_hdr *ip);
```

在 [src/ipv4.c](file:///c:/Users/zenboen/Tutorial/Networking_Fundamentals/src/ipv4.c) 實作：
```c
int ipv4_decrement_ttl(struct ipv4_hdr *ip)
{
    if (ip->ttl == 0)
        return -1;

    ip->ttl--;

    if (ip->ttl == 0)
        return -1;

    return 0;
}
```

---

### 2. `include/icmp.h`：定義常數與介面宣告

在 [include/icmp.h](file:///c:/Users/zenboen/Tutorial/Networking_Fundamentals/include/icmp.h) 中：
```c
#define ICMP_ECHO_REPLY     0
#define ICMP_ECHO_REQUEST   8
#define ICMP_TIME_EXCEEDED  11  // Type 11: Time Exceeded

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed));

void icmp_print_header(const struct icmp_hdr *icmp, size_t length);
int icmp_verify_checksum(const void *icmp, size_t length);
void icmp_handle(uint8_t *packet, size_t length);

// 模組化介面：接收並處理一般 ICMP 封包 (Echo Reply)
void icmp_receive(int fd, const uint8_t *frame, size_t len);

// 模組化介面：發送 ICMP Time Exceeded 報錯封包
void icmp_send_time_exceeded(int fd, const uint8_t *orig_frame, size_t orig_len);
```

---

### 3. `src/icmp.c`：組裝發送 Time Exceeded 與封裝 Echo Reply

在 [src/icmp.c](file:///c:/Users/zenboen/Tutorial/Networking_Fundamentals/src/icmp.c) 中實作兩個核心函式：

```c
void icmp_send_time_exceeded(int fd, const uint8_t *orig_frame, size_t orig_len)
{
    if (orig_len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr))
        return;

    const struct ethernet_hdr *orig_eth = (const struct ethernet_hdr *)orig_frame;
    const struct ipv4_hdr *orig_ip = (const struct ipv4_hdr *)(orig_frame + ETH_HEADER_LEN);

    uint8_t orig_ihl = (orig_ip->version_ihl & 0x0F) * 4;

    // 計算附帶的原封包資料長度：原 IP 標頭 + 原 Payload 前 8 bytes
    size_t orig_ip_payload_len = orig_len - ETH_HEADER_LEN - orig_ihl;
    size_t copy_payload_len = (orig_ip_payload_len >= 8) ? 8 : orig_ip_payload_len;
    size_t icmp_data_len = orig_ihl + copy_payload_len;

    size_t icmp_total_len = sizeof(struct icmp_hdr) + icmp_data_len;
    size_t reply_frame_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + icmp_total_len;

    uint8_t reply[2048];
    memset(reply, 0, reply_frame_len);

    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
    struct icmp_hdr *icmp_out = (struct icmp_hdr *)(reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *icmp_payload = reply + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr);

    // 1. 交換 MAC 地址
    memcpy(eth_out->dst, orig_eth->src, ETH_ADDR_LEN);
    memcpy(eth_out->src, orig_eth->dst, ETH_ADDR_LEN);
    eth_out->ethertype = htons(ETHERTYPE_IPV4);

    // 2. 組裝全新 IPv4 標頭
    ip_out->version_ihl = 0x45;
    ip_out->tos = 0;
    ip_out->total_length = htons(sizeof(struct ipv4_hdr) + icmp_total_len);
    ip_out->identification = htons(0);
    ip_out->flags_fragment = 0;
    ip_out->ttl = 64;
    ip_out->protocol = IPPROTO_ICMP;
    ip_out->src_ip = orig_ip->dst_ip; // 10.0.0.2
    ip_out->dst_ip = orig_ip->src_ip; // 10.0.0.1
    ip_out->checksum = 0;
    ip_out->checksum = ipv4_checksum(ip_out, sizeof(struct ipv4_hdr));

    // 3. 組裝 ICMP 標頭 (Type 11, Code 0, Unused 填 0)
    icmp_out->type = ICMP_TIME_EXCEEDED;
    icmp_out->code = 0;
    icmp_out->checksum = 0;
    icmp_out->identifier = 0;
    icmp_out->sequence = 0;

    // 4. 複製原 IP 標頭 + 前 8 bytes 負載
    memcpy(icmp_payload, orig_ip, icmp_data_len);

    // 5. 計算 ICMP Checksum
    icmp_out->checksum = ipv4_checksum(icmp_out, icmp_total_len);

    // 6. 送出
    ssize_t sent = write(fd, reply, reply_frame_len);
    if (sent < 0) {
        perror("[ICMP] write Time Exceeded failed");
    } else {
        printf("[ICMP] Time Exceeded (Type 11, Code 0) sent to ");
        ipv4_print_ip(ip_out->dst_ip);
        printf("\n");
        fflush(stdout);
    }
}
```

以及重構收納的 `icmp_receive`：
```c
void icmp_receive(int fd, const uint8_t *frame, size_t len)
{
    if (len < ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr))
        return;

    const uint8_t *payload = frame + ETH_HEADER_LEN;
    const struct ipv4_hdr *ip = (const struct ipv4_hdr *)payload;
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;

    const struct icmp_hdr *icmp_in = (const struct icmp_hdr *)(payload + ihl);
    size_t icmp_in_len = len - ETH_HEADER_LEN - ihl;

    icmp_print_header(icmp_in, icmp_in_len);

    if (icmp_in->type != ICMP_ECHO_REQUEST) {
        return;
    }

    uint8_t reply[2048];
    memcpy(reply, frame, len);

    struct ethernet_hdr *eth_out = (struct ethernet_hdr *)reply;
    struct ipv4_hdr *ip_out = (struct ipv4_hdr *)(reply + ETH_HEADER_LEN);
    uint8_t *icmp_out = reply + ETH_HEADER_LEN + ihl;
    size_t icmp_len = len - ETH_HEADER_LEN - ihl;

    // 交換 MAC 與 IP
    uint8_t temp_mac[ETH_ADDR_LEN];
    memcpy(temp_mac, eth_out->dst, ETH_ADDR_LEN);
    memcpy(eth_out->dst, eth_out->src, ETH_ADDR_LEN);
    memcpy(eth_out->src, temp_mac, ETH_ADDR_LEN);

    uint32_t temp_ip = ip_out->dst_ip;
    ip_out->dst_ip = ip_out->src_ip;
    ip_out->src_ip = temp_ip;
    ip_out->ttl = 64;
    ip_out->checksum = 0;
    ip_out->checksum = ipv4_checksum(ip_out, ihl);

    icmp_handle(icmp_out, icmp_len);

    ssize_t sent = write(fd, reply, len);
    if (sent < 0) {
        perror("[ICMP] write Echo Reply failed");
    } else {
        printf("[ICMP] Echo Reply sent (%ld bytes)\n", sent);
        fflush(stdout);
    }
}
```

---

### 4. `src/tap.c`：高度精簡的主分流邏輯

在 [src/tap.c](file:///c:/Users/zenboen/Tutorial/Networking_Fundamentals/src/tap.c) 中，原本臃腫的程式碼變得優雅清晰：

```c
            case ETHERTYPE_IPV4:
                if (payload_len >= sizeof(struct ipv4_hdr)) {
                    struct ipv4_hdr *ip = (struct ipv4_hdr *)payload;
                    ipv4_print_header(ip);

                    // 1. 扣減 TTL，若歸零則丟棄封包並回傳 ICMP Time Exceeded
                    if (ipv4_decrement_ttl(ip) != 0) {
                        printf("[IPv4] TTL Expired\n");
                        icmp_send_time_exceeded(fd, buffer, n);
                        fflush(stdout);
                        break; // 跳出 switch，不往下處理 protocol
                    }

                    // 2. IPv4 Protocol Dispatcher
                    switch (ip->protocol) {
                        case IPPROTO_ICMP:
                            icmp_receive(fd, buffer, n); // 一行委託 ICMP 模組！
                            break;
                        default:
                            break;
                    }
                }
                break;
```

---

# 實測驗證

### 測試 1：送出 TTL = 1 封包驗證 Time Exceeded

修改 `test/send_icmp.c` 中的 `ip->ttl = 1`：

```bash
# Terminal 1:
make
sudo ./network

# Terminal 2:
make send_icmp
sudo ./send_icmp
```

**Terminal 1 輸出（驗收成功）：**
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
TTL          : 1
Protocol     : 1
Checksum     : 0xa5de (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] TTL Expired
[ICMP] Time Exceeded (Type 11, Code 0) sent to 10.0.0.1
Frame length: 42 bytes
```

---

### 測試 2：驗證原有的 Echo Reply 依然正常運作

在 Host 端執行標準 Ping：
```bash
ping -c 1 10.0.0.2
```

**Terminal 1 輸出：**
```text
[ACCEPT]
Ethernet Frame
...
IPv4 Packet (TTL = 64)
...
ICMP Packet (Type = 8, Code = 0)
...
[ICMP] Echo Request received -> Generating Echo Reply
[ICMP] Echo Reply sent (98 bytes)
```
重構後不但保留了原先的自動 Ping-Pong 機制，還無縫擴充了 TTL 逾期處理！

---

# 目前網路堆疊架構演進

```text
                     Network Stack Architecture (Day 10)
 
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
                               (Drop & Notify)        (Protocol Dispatcher)
                                         │                     │
                                         ▼                     ▼
                                ICMP Time Exceeded      ICMP (Protocol = 1)
                                   (Type = 11)                 │
                                                      ┌────────┴────────┐
                                                      │                 │
                                                 Echo Request      Echo Reply
                                                  (Type = 8)       (Type = 0)
```

---

# 下一天：Day 11 預告

到目前為止，我們的 Network Stack 都是假設：**「所有收到的封包，目的地都是要給我的」**。
但在真實的網際網路中，路由器每天面對的成千上萬個封包，目的地根本都不是自己！

下一天我們將踏出邁向「真實路由器」最重要的一步：**Routing（路由選擇）**：
* 封包目的地是給我自己？還是要幫忙轉發出去？
* 如果要轉發，應該丟給哪一個 **Gateway**？誰是 **Next Hop**？
* 第一次實作 **Routing Table（路由表）** 與前綴匹配！
