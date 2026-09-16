# Day 20：TCP Data Transfer (Part 1) — 接收 Payload 並回 ACK

在 Day 19 中，我們成功完成了 TCP 三向交握（Three-Way Handshake），連線正式進入了 `TCP_ESTABLISHED` 狀態。

今天（Day 20），我們邁出了通往真實世界網路應用的最關鍵一步：**開始接收應用層的真正資料（Payload），精準解析長度，並回傳確認收到的純 ACK 封包！**

```text
Client                                             Server
  │                                                  │
  │ 1. 三向交握 (SYN -> SYN-ACK -> ACK)              │
  │<================================================>│  (連線建立：ESTABLISHED)
  │                                                  │
  │ 2. HTTP Request (SEQ=1001, ACK=5001)             │
  │    GET / HTTP/1.1\r\nHost: 10.0.0.2\r\n\r\n      │
  ├─────────────────────────────────────────────────>│  (解析 Data Offset，算出 Payload 長度)
  │                                                  │  (印出 [TCP DATA] 內容)
  │                                                  │  (計算 ACK 號碼 = 1001 + 32 = 1033)
  │                                                  │
  │ 3. ACK (SEQ=5001, ACK=1033)                      │
  │<─────────────────────────────────────────────────┤  (Server 回傳純 ACK，確認全部收妥！)
  │                                                  │
```

---

# 今日學習目標與成果

- [x] **解析 TCP Data Offset 動態標頭長度**：
  - 理解為什麼 TCP 標頭長度不固定（Options 選項的存在，如 MSS、Window Scale、SACK 等）。
  - 掌握高 4 位元提取與乘 4 換算（`tcp_hdr_len = (tcp->data_offset >> 4) * 4`）。
- [x] **計算 Payload 起始指標與長度**：
  - 指標偏移：`payload = (uint8_t *)tcp + tcp_hdr_len`。
  - 長度計算：`payload_len = ntohs(ip->total_length) - ip_hdr_len - tcp_hdr_len`。
- [x] **實作 Payload 輸出器 `tcp_dump_payload()`**：
  - 使用安全二進位輸出函式 `fwrite`，避免因缺少 `\0` 造成記憶體越界。
- [x] **理解 ESTABLISHED 狀態下的 ACK 旗標特性**：
  - 深入探討全雙工通訊中的「搭便車原則（Piggybacking）」：連線建立後，幾乎所有封包都帶著 ACK。
- [x] **實作純 ACK 發送器 `tcp_send_ack()`**：
  - 封裝無 Payload 的純 ACK 標頭（Flags = `TCP_ACK`）。
  - 依據收到的位元組數推進確認序號：`conn->ack = received_seq + payload_len`。
- [x] **撰寫自動化端到端資料傳輸測試程式 (`test/send_tcp_data.c`)**：
  - 自動完成交握 $\rightarrow$ 發送 HTTP GET 請求 $\rightarrow$ 等待並驗證 Server 回傳的累積確認 ACK。

---

# 核心概念深入剖析

### 1. 為什麼 TCP 標頭長度不固定？

在 UDP 中，標頭永遠固定為 **8 Bytes**。
但在 TCP 中，RFC 793 為了保留未來的擴展彈性，在固定的 20 Bytes 之後預留了 **Options（選項欄位）**：

| Option 名稱 | 主要目的 | 常見出現時機 |
| :--- | :--- | :--- |
| **MSS (Maximum Segment Size)** | 協商最大封包大小，避免中間路由器 IP 分片 | SYN 握手階段 |
| **Window Scale** | 將 16-bit 接收視窗突破 64 KB 限制，支援現代 G 級頻寬 | SYN 握手階段 |
| **SACK (Selective ACK)** | 允許接收端回報局部收到的區塊，掉一包只重傳一包 | 資料傳輸階段 |
| **Timestamps** | 精準計算延遲 RTT，防止極速網路下的序號回繞（PAWS） | 幾乎每個資料封包 |

因此，TCP 標頭長度可在 **20 ~ 60 Bytes** 之間動態浮動，必須透過 `data_offset` 精準定位資料本體。

---

### 2. 為什麼 Data Offset 要右移 4 位再乘以 4？

在 TCP Header 的第 12 個 Byte 結構如下：
```text
+-+-+-+-+-+-+-+-+
|Data Offset|Res|
+-+-+-+-+-+-+-+-+
 7 6 5 4 3 2 1 0
```
1. **右移 4 位 (`>> 4`)**：
   因為長度值存放在高 4 位（bits 7~4），低 4 位為保留位（Reserved），右移能取出數值。
2. **乘以 4 (`* 4`)**：
   4 個 bit 最大只能表示 $15$。若單位是 Byte，連最基本的 20 Bytes 都裝不下。因此協定規定**以「32-bit (4 Bytes) 字組」為計數單位**：
   - 數值 `5`：$5 \times 4 = 20\text{ Bytes}$（無 Options）
   - 數值 `8`：$8 \times 4 = 32\text{ Bytes}$（包含 12 Bytes Options）

---

### 3. ACK Number 的計算規則（Cumulative ACK 累積確認）

TCP 是位元組流（Byte Stream）協定，序號代表的是「第幾個 Byte」：
- 客戶端送出起始序號為 `SEQ = 1001` 的 HTTP GET 請求。
- 請求內容 `"GET / HTTP/1.1\r\nHost: 10.0.0.2\r\n\r\n"` 共 **32 Bytes**。
- 客戶端消耗的序號空間是 `1001 ~ 1032`。
- 伺服器回覆的確認號碼公式：
  $$\text{Next ACK} = \text{received\_seq} + \text{payload\_len} = 1001 + 32 = \mathbf{1033}$$
- 這代表伺服器向客戶端宣告：**「1032 以前的位元組我全部收妥，請你下次從 1033 開始送！」**

---

# 核心程式碼實作

### 1. Payload 輸出與純 ACK 發送器 (`src/tcp.c`)

```c
void tcp_dump_payload(const uint8_t *data, size_t len)
{
    printf("\n[TCP DATA]\n");
    fwrite(data, 1, len, stdout);
    printf("\n");
    fflush(stdout);
}

int tcp_send_ack(int fd, struct tcp_socket *conn)
{
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 1. TCP Header (純 ACK，無 Payload)
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq);
    tcp->ack = htonl(conn->ack);
    tcp->data_offset = (5 << 4);
    tcp->window = htons(4096);
    tcp->flags = TCP_ACK;
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    // 2. IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(2002);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 3. Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send ACK failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 4. 發送至虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write ACK failed");
        return -1;
    }
    printf("[TCP] Sent ACK: SEQ=%u, ACK=%u\n", conn->seq, conn->ack);
    return 0;
}
```

### 2. 在 `tcp_receive()` 處理資料傳輸

```c
    // 計算 TCP Header 長度與 Payload 位置
    size_t tcp_hdr_len = (tcp->data_offset >> 4) * 4;
    const uint8_t *payload = (const uint8_t *)tcp + tcp_hdr_len;
    uint16_t ip_total_len = ntohs(ip->total_length);
    size_t payload_len = 0;
    if (ip_total_len >= ip_hdr_len + tcp_hdr_len) {
        payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
    }

    ...

    if (conn->state == TCP_ESTABLISHED) {
        if (payload_len > 0) {
            printf("\n[TCP] Received Payload, Length = %zu bytes\n", payload_len);
            tcp_dump_payload(payload, payload_len);

            // 更新 ACK 號碼 (累加收到的 byte 數)
            uint32_t received_seq = ntohl(tcp->seq);
            conn->ack = received_seq + payload_len;

            // 回傳 ACK 封包
            tcp_send_ack(fd, conn);
        }
        return;
    }
```

---

# 實機驗收步驟

### 步驟 1：啟動協定棧
在第一個終端機執行：
```bash
sudo ./network
```

### 步驟 2：執行自動化資料傳輸測試
在第二個終端機執行：
```bash
sudo ./send_tcp_data
```

---

# 預期輸出結果

#### `./network` 終端機：
```text
[TCP] ACK Received! Handshake Complete!
[TCP] Connection Established: State -> ESTABLISHED

[TCP] Received Payload, Length = 32 bytes

[TCP DATA]
GET / HTTP/1.1
Host: 10.0.0.2

[TCP] Sent ACK: SEQ=5001, ACK=1033
```

#### `./send_tcp_data` 測試終端機：
```text
[1/3 Client] 發送 TCP SYN (SEQ=1000) 到 tap0...
[2/3 Client] 等待 Server 回傳 SYN-ACK...
[2/3 Client] 收到 Server 的 SYN-ACK！(Server SEQ=5000, ACK=1001)
[3/3 Client] 發送最終 ACK (SEQ=1001, ACK=5001) 完成三向交握！

[4/4 Client] 發送 HTTP Request (32 bytes) 到 tap0...
[5/4 Client] 等待 Server 回傳資料的 ACK...

========================================
[Client] 成功收到 Server 回傳的 ACK！(ACK=1033)
[Client] 驗證成功！Server 正確確認了全部 32 bytes 資料 (1001 + 32 = 1033)！
========================================
```

---

# 今日總結與明日預告

今天我們成功實現了從 TCP 標頭解析 Payload，並根據收到的長度推進 ACK 序號且回覆純 ACK，這是整個協定棧能處理真實資料傳輸的起點！

### Day 21 預告：
今天我們是**被動接收資料並確認**。明天（Day 21），我們將實作 **TCP Send（主動傳送資料）**！
當收到客戶端的 `GET / HTTP/1.1` 時，伺服器將親自封裝並主動回傳：
```text
HTTP/1.1 200 OK
Content-Length: 13

Hello, World!
```
讓客戶端第一次收到我們自製 TCP Stack 吐出的網頁內容！
