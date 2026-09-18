# Day 19：TCP Three-Way Handshake (Part 2) — 收到 ACK，進入 ESTABLISHED

在 Day 18 中，我們的網路協定棧已經能夠成功回覆 `SYN-ACK`，雙方建立了連線記錄並停留在 `TCP_SYN_RECEIVED`。

今天（Day 19），我們完成 TCP 連線建立中最關鍵、也是最後的一哩路：
**收到客戶端回傳的純 ACK 封包，驗證序號，正式將狀態推進至 `TCP_ESTABLISHED`！**

```text
Client                                  Server
  │                                       │
  │ 1. SYN (SEQ=1000)                     │
  ├──────────────────────────────────────>│  (找到 LISTEN Socket -> 建立新連線 -> SYN_RECEIVED)
  │                                       │
  │ 2. SYN-ACK (SEQ=5000, ACK=1001)       │
  │<──────────────────────────────────────┤  (Server 發送 SYN-ACK)
  │                                       │
  │ 3. ACK (SEQ=1001, ACK=5001)           │
  ├──────────────────────────────────────>│  <-- 【今日核心成果！】
  │                                       │
  │                                       ▼
  │                                 [ESTABLISHED]
  │                             (TCP 連線正式成功建立！)
```

這代表我們的 TCP 協定棧不再只是「聽得見」和「說得出」，而是**正式具備了最小化三向交握狀態轉換能力**，這是自製 TCP/IP 協定棧中非常重要的里程碑！

---

# 今日學習目標與成果

- [x] **實作連線四元組查找器 `tcp_find_connection()`**：
  - 依據 `{src_ip, dst_ip, src_port, dst_port}` 四元組精確定位特定的連線 Socket。
  - 理解為什麼不能寫死只查 `TCP_SYN_RECEIVED`，而是必須涵蓋未來所有的連線狀態（如 `ESTABLISHED`、`FIN_WAIT` 等）。
- [x] **純 ACK 封包辨識與處理**：
  - 在 `tcp_receive()` 中正確過濾純 ACK 旗標：`(tcp->flags & TCP_ACK) && !(tcp->flags & TCP_SYN)`。
- [x] **嚴格驗證 ACK Number**：
  - 伺服器先前送出的 ISN 為 `5000`，因 SYN 佔用 1 個序號空間，客戶端回傳的 ACK 必須恰好為 `expected_ack = conn->seq + 1`（即 `5001`）。
- [x] **完成狀態機轉換與序號推進**：
  - 將狀態切換為 `conn->state = TCP_ESTABLISHED`。
  - 執行 `conn->seq++`，將伺服器序列號推進至 `5001`，為後續資料傳輸鋪路。
- [x] **實作 `tcp_accept()` 雛形**：
  - 為應用層（如未來的 HTTP Server）提供取得已就緒連線的介面。
  - 目前版本只是回傳第一個 `TCP_ESTABLISHED` 連線，尚未實作真實 OS 中的 accept queue、blocking wakeup 與 backlog 管理。
- [x] **撰寫三向交握自動測試程式 (`test/send_tcp_handshake.c`)**：
  - 模擬客戶端自動依序完成：發送 SYN $\rightarrow$ 等待接收 SYN-ACK $\rightarrow$ 發送最終 ACK。
- [x] **實機全鏈路驗收成功**：
  - Socket Table 成功由 `SYN_RECEIVED` 躍升為 `ESTABLISHED`！

---

# 核心概念深入剖析

### 1. 為什麼連線查找器必須比對「四元組（4-tuple）」？

在伺服器上，可能會同時有數以萬計的連線正在運作。TCP 是如何將收到的封包精準派送給對應的連線呢？

答案就是 **4-tuple**：
$$\text{\{ 來源 IP, 目的 IP, 來源 Port, 目的 Port \}}$$

在全世界的網際網路中，這四個欄位的組合對於一條 TCP 連線來說是**絕對唯一**的。

#### 為什麼不能只找 `state == TCP_SYN_RECEIVED`？
若將 `tcp_find_connection()` 限制死在 `SYN_RECEIVED`：
1. 握手最後一步收到 ACK 時確實找得到。
2. 但一旦連線進入 `ESTABLISHED`，隨後客戶端傳送資料（HTTP 請求）或關閉連線（FIN）時，封包再次進來，查找器就會回傳 `NULL`，導致封包被無辜丟棄！
3. 因此：**`tcp_find_connection()` 只負責「認人（4-tuple）」，狀態是否正確則交由各處理階段（如 `tcp_receive()`）來驗證。**

---

### 2. 為什麼 Client 回傳的 ACK 必須是 5001？

在昨天的交握中，伺服器送出：
- `SEQ = 5000`
- `Flags = SYN | ACK`

雖然此封包沒有包含任何應用層 Data（Payload 長度為 0），但在 TCP 規範（RFC 793）中：
> **`SYN` 與 `FIN` 控制旗標各自在邏輯上消耗 1 個 Sequence Number。**

因此，客戶端收到 `SEQ = 5000` 的 SYN 後，必須回傳 `ACK = 5001`。
這代表客戶端向伺服器宣告：
> *「我已經完整收下了你的 SYN（序號 5000），接下來請你從 5001 號開始發送資料給我！」*

---

### 3. 為什麼一定要「三次」交握？不能兩次嗎？

如果只有兩次交握（Client 發 SYN，Server 回 SYN-ACK 即建立）：
- **假想情境**：Client 發送的第一個 SYN 在網路節點中塞車延遲了，Client 超時後重新發了第二個 SYN 並完成通訊後斷線。
- 很久之後，那個塞車的舊 SYN 終於抵達 Server。
- 如果只要兩次交握，Server 收到這個舊 SYN 就立刻單方面建立連線並分配資源等待 Client。
- 但 Client 根本沒有要連線，不會理會 Server 的回覆，這條連線就會變成**半死連線（Half-Open Connection）**，造成伺服器資源浪費。

有了**第三次 ACK**：
- Server 必須等到 Client 的最終確認才會將連線標記為 `ESTABLISHED`。
- 若是舊的過期 SYN，Client 收到 Server 的 SYN-ACK 後會立刻發現異常並發送 `RST` 終止連線，有效防止歷史連線造成的混亂與資源耗盡。

---

# 核心程式碼實作

### 1. 介面擴充 (`include/tcp.h`)

新增四元組查找函式與連線受理函式宣告：

```c
/* 連線查找與管理 */
struct tcp_socket* tcp_find_connection(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port);
struct tcp_socket* tcp_accept(void);
```

---

### 2. 四元組查找與 `tcp_accept()` 實作 (`src/tcp.c`)

```c
struct tcp_socket* tcp_find_connection(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        // 排除未使用的槽位(CLOSED)與正在迎賓的門口(LISTEN)
        if (tcp_table[i].state != TCP_CLOSED && tcp_table[i].state != TCP_LISTEN) {
            if (tcp_table[i].src_ip == src_ip &&
                tcp_table[i].dst_ip == dst_ip &&
                tcp_table[i].src_port == src_port &&
                tcp_table[i].dst_port == dst_port) {
                return &tcp_table[i];
            }
        }
    }
    return NULL;
}

struct tcp_socket* tcp_accept(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_ESTABLISHED) {
            return &tcp_table[i];
        }
    }
    return NULL;
}
```

---

### 3. ACK 處理與狀態推進 (`src/tcp.c`)

在 `tcp_receive()` 中新增處理邏輯：

```c
    if ((tcp->flags & TCP_ACK) && !(tcp->flags & TCP_SYN)) {
        uint16_t src_port = ntohs(tcp->src_port);
        uint16_t dst_port = ntohs(tcp->dst_port);

        // 1. 透過四元組尋找連線
        struct tcp_socket *conn = tcp_find_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] No matching connection for ACK -> DROP\n");
            return;
        }

        // 2. 檢查狀態是否為 SYN_RECEIVED
        if (conn->state == TCP_SYN_RECEIVED) {
            uint32_t ack_num = ntohl(tcp->ack);
            uint32_t expected_ack = conn->seq + 1; // 5000 + 1 = 5001

            // 3. 驗證對方的 ACK 號碼
            if (ack_num != expected_ack) {
                printf("[TCP] Invalid ACK number: %u (expected %u) -> DROP\n", ack_num, expected_ack);
                return;
            }

            // 4. 三向交握圓滿成功！切換狀態
            conn->state = TCP_ESTABLISHED;
            conn->seq++; // 消耗 SYN 序號，推進至 5001

            printf("\n========================================\n");
            printf("[TCP] ACK Received! Handshake Complete!\n");
            printf("[TCP] Connection Established: State -> ESTABLISHED\n");
            printf("========================================\n\n");

            tcp_dump_table();
            return;
        }
    }
```

---

# 實測驗收結果

### 終端機執行測試：
在終端機 A 執行協定棧：
```bash
sudo ./network
```
在終端機 B 執行自動交握測試工具：
```bash
sudo ./send_tcp_handshake
```

### 伺服器輸出日誌：

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
Destination Port : 8080
SEQ              : 1000
ACK              : 0
Data Offset      : 5 (Header Length: 20 bytes)
Flags            : 0x02 [ SYN ]
                   SYN = 1, ACK = 0, FIN = 0, RST = 0
Window           : 4096
Checksum         : 0x0000
Urgent Pointer   : 0
----------------------------------------
[TCP] Incoming SYN on Port 8080

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
[01] State: SYN_RECEIVED | 10.0.0.1:52144 -> 10.0.0.2:8080
========================

[TCP] Sent SYN-ACK: SEQ=5000, ACK=1001
Frame length: 54 bytes

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
Checksum     : 0x62e4 (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] Local Delivery (for me)

TCP Packet
----------------------------------------
Source Port      : 52144
Destination Port : 8080
SEQ              : 1001
ACK              : 5001
Data Offset      : 5 (Header Length: 20 bytes)
Flags            : 0x10 [ ACK ]
                   SYN = 0, ACK = 1, FIN = 0, RST = 0
Window           : 4096
Checksum         : 0x0000
Urgent Pointer   : 0
----------------------------------------

========================================
[TCP] ACK Received! Handshake Complete!
[TCP] Connection Established: State -> ESTABLISHED
========================================

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
[01] State: ESTABLISHED  | 10.0.0.1:52144 -> 10.0.0.2:8080
========================

Frame length: 54 bytes
```

連線表正式由 `SYN_RECEIVED` 邁入 `ESTABLISHED`，三向交握完美成功！

---

# Day 20 預告：TCP Data Transfer（資料傳輸）

TCP 連線建立的最終目的，是為了穩定可靠地傳遞應用層資料。

在 Day 20 中，我們將實作：
- 解析帶有 Payload 的 TCP 封包（例如 HTTP 的 `GET / HTTP/1.1`）。
- 計算收到的資料長度並印出內容。
- 更新連線的確認序號（`conn->ack += data_len`）並回傳 Data ACK，告訴客戶端我們已經確實收下資料！
