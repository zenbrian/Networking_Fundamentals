# Day 21：TCP Data Transfer (Part 2) — 主動送出 Payload

在 Day 20 中，我們的 TCP Stack 已經能夠接收客戶端發送的應用層資料（Payload），並成功計算出對應的累積確認號（ACK Number）回傳純 ACK。

今天（Day 21），我們迎來了 TCP 實作的重大里程碑：**讓我們的 TCP Stack 第一次具備主動發送應用層資料（Send Payload）的能力，完成教學版的最小雙向資料傳輸！**

```text
Client                                             Server
  │                                                  │
  │ 1. 三向交握 (SYN -> SYN-ACK -> ACK)              │
  │<================================================>│  (連線建立：ESTABLISHED)
  │                                                  │
  │ 2. HTTP Request (SEQ=1001, ACK=5001)             │
  │    GET / HTTP/1.1\r\nHost: 10.0.0.2\r\n\r\n      │
  ├─────────────────────────────────────────────────>│  (Server 收到 34 Bytes Payload)
  │                                                  │
  │ 3. ACK 回條 (SEQ=5001, ACK=1035)                 │
  │<─────────────────────────────────────────────────┤  (純 ACK：確認收到請求)
  │                                                  │
  │ 4. TCP Payload Response (SEQ=5001, ACK=1035)     │
  │    "Hello from My TCP Stack\n" (24 Bytes)        │
  │<─────────────────────────────────────────────────┤  【tcp_send()】主動送出 Payload！
  │                                                  │  (Server SEQ 推進：5001 + 24 = 5025)
  │                                                  │
```

---

# 今日學習目標與成果

- [x] 區分純 ACK 封包與帶 Payload 的資料封包。
- [x] 實作 `tcp_send()`，封裝 TCP Payload。
- [x] 動態計算封包長度與 IPv4 `total_length`。
- [x] 使用 `TCP_ACK | TCP_PSH` 送出資料。
- [x] 傳送資料後推進 `conn->seq += len`。
- [x] 收到 HTTP GET 後主動回傳 `Hello from My TCP Stack\n`。
- [x] 驗證 Client 成功收到 Server 主動送出的資料。

---

# 核心概念深入剖析

### 1. 為什麼需要獨立的 `tcp_send()` 函式？

我們之前已經有了 `tcp_send_syn_ack` 與 `tcp_send_ack`，為什麼不能直接用它們送資料？

| 比較維度 | `tcp_send_ack()` | `tcp_send()` (今日主角) |
| :--- | :--- | :--- |
| **用途** | 傳輸層協定控制（純確認回條） | 應用層通用資料傳輸（送字串、HTML、檔案） |
| **封包長度** | 固定 54 Bytes（無 Payload） | 動態：`54 Bytes + len` |
| **記憶體配置** | 固定長度陣列 | 需動態容納應用層資料並 `memcpy` |
| **序號推進** | **不消耗序號**（`conn->seq` 不變） | **消耗序號**（`conn->seq += len`） |
| **呼叫者角色** | TCP 協定層自動觸發 | 提供給上層應用程式的輸出介面（如 `send()`） |

---

### 2. 什麼是 Buffer？記憶體空間如何切分？

在網路發送中，**Buffer（緩衝區）** 就是一條連續的記憶體紙帶：
```text
uint8_t buffer[total_len];
```
各層協定透過指標位移，在紙帶的指定位置填寫標籤：

```text
┌─────────────────┬─────────────────┬────────────────┬────────────────────────┐
│ Ethernet Header │   IPv4 Header   │   TCP Header   │   Payload (應用層資料) │
│ (14 Bytes)      │   (20 Bytes)    │   (20 Bytes)   │   (len Bytes)          │
└─────────────────┴─────────────────┴────────────────┴────────────────────────┘
▲                 ▲                 ▲                ▲
eth               ip                tcp              payload
```

![Day21 tcp_send Buffer Layout 與 Payload 封裝圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day21/Day21_1.png)

* **`data`**：應用程式想傳送的字串或資料（來源記憶體）。
* **`payload`**：封包紙帶中 TCP 標頭後方的起始指標（目的記憶體）。
* **`memcpy(payload, data, len)`**：將貨物裝箱打包進封包中。

> **注意：`data_offset` 絕不能加上 `len`！**
> `data_offset` 代表「TCP 標頭本身的長度（以 32-bit 字組為單位）」，告訴接收端標頭在哪裡結束、資料從哪裡開始。若加上 `len`，接收端會誤以為標頭很長，導致找不到資料。

---

### 3. TCP_PSH 旗標的意義

TCP 可能會為了吞吐量與效率而暫存資料。`TCP_PSH`（Push）不是「應用層訊息邊界」的保證，而是一個提示：希望接收端不要為了等待更多資料而延遲，能儘快把目前已收到的資料交給應用程式。

在 Wireshark 側錄中，許多 HTTP Request/Response、終端機命令等即時互動封包，常會看到 `[PSH, ACK]`。

---

### 4. 獨立確認 vs 捎帶確認（Piggybacking）

當 Server 收到 Client 的資料時：
* **獨立確認（Separate ACK）**：
  先送一包純 ACK（54 Bytes）確認收妥，隨後再送一包資料（88 Bytes）。
  （適用於 Server 需要耗時運算/查詢資料庫的場景，避免 Client 逾時重傳）。
* **捎帶確認（Piggybacking）**：
  因為 `tcp_send()` 本身就帶有 `TCP_ACK` 旗標與確認號碼 `tcp->ack = htonl(conn->ack)`，若直接送資料封包，就同時完成了「回覆資料」與「確認請求」，節省一個封包。

本日教學版為了觀察清楚，採用「先送純 ACK，再送資料」的方式；真實 TCP stack 常會透過 **Delayed ACK（延遲確認）** 判斷是否能合併成 Piggyback：若應用程式很快回傳就合併，若應用程式較慢就先發純 ACK。

---

# 核心程式碼實作

### 1. 通用資料發送引擎 (`src/tcp.c`)

```c
int tcp_send(int fd, struct tcp_socket *conn, const uint8_t *data, size_t len)
{
    // 1. 計算總長度：L2 (14) + L3 (20) + L4 (20) + 應用層資料長度 (len)
    size_t total_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + len;
    uint8_t buffer[total_len];
    memset(buffer, 0, total_len);
    
    // 2. 切割記憶體指標
    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *payload = buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);
    
    // 3. 搬移應用層資料到 Payload 區域
    if (len > 0 && data != NULL) {
        memcpy(payload, data, len); 
    }

    // 4. 封裝 TCP Header
    tcp->src_port = htons(conn->dst_port); // Server 本地埠號
    tcp->dst_port = htons(conn->src_port); // Client 遠端埠號
    tcp->seq = htonl(conn->seq); 
    tcp->ack = htonl(conn->ack); 
    tcp->data_offset = (5 << 4);           // 標頭固定 20 Bytes (5 words)
    tcp->flags = TCP_ACK | TCP_PSH;        // ACK + PSH
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    // 5. 封裝 IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + len);
    ip->identification = htons(2003);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 6. 封裝 Ethernet Header (透過 ARP 表查 MAC)
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send Data failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);
    
    // 7. 寫出到 TAP 虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write Data failed");
        return -1;
    }
    
    // 8. ★ 最關鍵的一步：推進 Sequence 序號！
    conn->seq += len;
    printf("[TCP] Sent Data: %zu bytes | New SEQ=%u, ACK=%u\n", len, conn->seq, conn->ack);
    return 0;
}
```

### 2. 測試回應包裝與觸發 (`src/tcp.c`)

```c
void tcp_send_data(int fd, struct tcp_socket *conn)
{
    const char *msg = "Hello from My TCP Stack\n";
    tcp_send(fd, conn, (const uint8_t *)msg, strlen(msg));
}

// 在 tcp_receive() 的 ESTABLISHED 區塊中：
if (conn->state == TCP_ESTABLISHED) {
    if (payload_len > 0) {
        printf("\n[TCP] Received Payload, Length = %zu bytes\n", payload_len);
        tcp_dump_payload(payload, payload_len);

        // 推進確認序號
        uint32_t received_seq = ntohl(tcp->seq);
        conn->ack = received_seq + payload_len;

        // 回傳純 ACK
        tcp_send_ack(fd, conn);

        // 主動回覆資料！
        tcp_send_data(fd, conn);
    }
    return;
}
```

---

# 驗收與執行結果

### 步驟 1：啟動 TCP Stack
```bash
sudo ./network
```

### 步驟 2：執行端對端自動化測試
```bash
sudo ./send_tcp_data
```

---

### 測試執行日誌剖析

#### `./network` 終端機：
```text
[TCP] Received Payload, Length = 34 bytes

[TCP DATA]
GET / HTTP/1.1
Host: 10.0.0.2

[TCP] Sent ACK: SEQ=5001, ACK=1035
[TCP] Sent Data: 24 bytes | New SEQ=5025, ACK=1035
Frame length: 88 bytes
```

#### `./send_tcp_data` 測試終端機：
```text
[Client] 成功收到 Server 回傳的 ACK！(ACK=1035)
[6/4 Client] 等待 Server 回傳應用層資料 (Data Segment)...

[Client 成功收到 Server 回覆！]
長度：24 Bytes | SEQ=5001, ACK=1035
內容：Hello from My TCP Stack

================================================
```

#### 💡 同一條連線內的關鍵觀察（Sequence 累加驗證）：

![Day21 Server SEQ 推進圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day21/Day21_2.png)

如果在同一條 `TCP_ESTABLISHED` 連線尚未關閉時，連續呼叫 `tcp_send()` 傳送資料，可以觀察到 Server 端 `conn->seq` 會依照 Payload 長度持續推進：
```text
[TCP] Sent ACK: SEQ=5025, ACK=1035
[TCP] Sent Data: 24 bytes | New SEQ=5049, ACK=1035
```
* 第一次傳送：$5001 + 24 = 5025$
* 第二次傳送：$5025 + 24 = 5049$
* 這證明了 `conn->seq += len` 能在同一條 TCP 位元組流（Byte Stream）中維持連續性。

> 若是重新建立一條新的 TCP 連線，真實 TCP 會為新連線建立新的 TCB 並選擇新的 ISN；不應把舊連線的 `seq` 直接延續到新連線。

---

# 今日總結與明日預告

今天我們完成了自製網路協定棧至關重要的里程碑 —— **主動封裝並傳送應用層資料**。現在我們的 TCP Stack 已經能夠示範最小化的 HTTP-like Request-Response 雙向通訊流程；不過它仍是教學版，尚未補齊 TCP Checksum、分段、重傳、擁塞控制、完整關閉流程與錯誤處理。

### Day 22 預告：
在真實不可靠的網際網路中，封包可能會**延遲、重複、甚至亂序（Out-of-Order）到達**：
* 預期順序：`SEQ=1001` $\rightarrow$ `SEQ=1101` $\rightarrow$ `SEQ=1201`
* 實際收到：`SEQ=1201` $\rightarrow$ `SEQ=1001` $\rightarrow$ `SEQ=1101`

明天我們將進入 TCP 最核心的可靠性保證機制：
**實作 TCP Receive Buffer 與重組（TCP Reassembly）**，學會如何將亂序的封包按照 Sequence Number 拼回最原始、正確的資料流！
