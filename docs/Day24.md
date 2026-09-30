# Day 24：TCP Four-Way Teardown — 優雅地關閉 TCP 連線

到昨天為止，我們的自製 TCP 協定棧已經具備了三向交握、雙向資料傳輸、亂序重組以及超時/快速重傳等可靠傳輸能力。

但如果一條連線資料傳輸完畢後，發送端與接收端該如何**優雅且安全地結束連線**？
如果只是單方面直接切斷，另一端可能還有資料正塞在緩衝區準備傳送，這將導致資料丟失。

今天（Day 24），我們為 TCP Stack 補上具備**被動關閉（Passive Close）**與**主動關閉（Active Close）**路徑的教學版**四向揮手終止機制（Four-Way Teardown）**，並實作 **2 秒 `TIME_WAIT` 防禦留守機制**，完成 TCP 連線生命週期中最重要的關閉流程。

![Day24 TCP 連線生命週期：三向交握與四向揮手時序圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day24/Day24_1.png)

---

# 今日學習目標與成果

- [x] 理解 TCP 四向揮手與 Half-Close 概念。
- [x] 擴充 TCP 關閉狀態：`FIN_WAIT_1`、`FIN_WAIT_2`、`CLOSE_WAIT`、`LAST_ACK`、`TIME_WAIT`。
- [x] 實作 `tcp_send_fin()`，送出 `FIN | ACK` 封包並推進 `conn->seq`。
- [x] 實作被動關閉路徑：`CLOSE_WAIT → LAST_ACK → CLOSED`。
- [x] 實作主動關閉入口 `tcp_close()`。
- [x] 實作主動關閉路徑：`FIN_WAIT_1 → FIN_WAIT_2 → TIME_WAIT`。
- [x] 實作教學版 2 秒 `TIME_WAIT` 計時器。
- [x] 在 `TIME_WAIT` 期間收到重傳 FIN 時補發最後 ACK。
- [x] 使用 `send_tcp_fin` 與 `send_tcp_active_close` 驗證雙向關閉流程。

---

# 核心概念深入剖析

### 1. 為什麼建立連線只要三次，關閉卻要四次？

三向交握時，雙方都是「準備好開始」的狀態，因此 Server 可以把自己的 `SYN` 與確認對方的 `ACK` 合併在同一個封包送出（即 `SYN-ACK`）。

但關閉連線時，情況完全不同：**TCP 是一條「全雙工（Full-Duplex）」的通道**，「我聽你說」與「我對你說」是兩條完全獨立的單向傳輸路徑。

```text
通話情境比喻：

1. Client：「我要講的事情講完了，我準備掛電話囉！」(Client 送出 FIN)

2. Server：「好，我知道你說完了，我確認收到。」(Server 送出 ACK，進入 CLOSE_WAIT) 此時電話還不能掛！因為 Server 手邊還有報告文件正在傳真給 Client！Client 閉上嘴巴不再發言，但耳朵還緊貼著聽筒繼續接收 Server 的資料。

3. Server：「好！我這邊的文件也全部傳真完畢，我也要掛電話了！」(Server 送出 FIN)

4. Client：「收到，掰掰！」(Client 回送最終 ACK，主動關閉端進入 TIME_WAIT，之後再釋放連線)
```

因為雙方結束通話的時機通常不一致，所以被動方必須**先回 ACK**，等自己資料也傳完後**再多送一次 FIN**，這就是為什麼必須是「四次」揮手。

---

### 2. 為什麼 FIN 和 SYN 都沒有帶 Payload，卻都要「+1 消耗序號」？

在 TCP 封包中：
* **純 ACK 封包**：不攜帶資料，也不佔用 TCP 序號空間；即使遺失，通常也不會像資料段或 FIN 一樣被單獨重傳。
* **SYN 與 FIN**：代表著「建立連線」與「終止傳輸」的**重大狀態控制指令**！
  TCP 規範規定：**凡是需要被對方確認、需要可靠保證抵達的控制旗標，必須在序號空間中佔用 1 個 Byte**。
  因此接收端收到序號為 `SEQ` 的 FIN 時，回傳的確認號必須精確為 `ACK = SEQ + 1`，以此向發送端保證「我確實收到並處理了你的 FIN」！

---

### 3. 主動關閉三部曲：`FIN_WAIT_1`、`FIN_WAIT_2`、`TIME_WAIT` 差在哪裡？

這三個狀態全都是**「主動先提出掛電話的那一方（主動關閉端）」**才會經歷的狀態，代表著掛電話的進度條：

![Day24 主動關閉端狀態旅程圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day24/Day24_2.png)

1. **`FIN_WAIT_1`**
   * **觸發動作**：主動方送出第 1 揮 `FIN`。
   * **當下心態**：「我講完了，準備掛電話囉！但不知道你有沒有收到？我正在等你的 ACK 確認。」

2. **`FIN_WAIT_2`**
   * **觸發動作**：收到被動方回覆的第 2 揮 `ACK`。
   * **當下心態**：「你確認知道我講完了，但換你手邊可能還有資料要講！我先安靜聽你說，等你的 FIN。」

3. **`TIME_WAIT`**
   * **觸發動作**：收到被動方的第 3 揮 `FIN`，並回覆第 4 揮最終 `ACK`。
   * **當下心態**：「雙方都講完了，最後的 ACK 也送出了。但我怕最後這個 ACK 在路上掉了導致你重傳 FIN，所以我在原地留守 2 秒防禦，確認沒事後才正式解散釋放（CLOSED）。」

---

### 4. 教學版 `TIME_WAIT` vs 真實 TCP

| 比較維度 | 真實 TCP | 本專案教學版 |
| :--- | :--- | :--- |
| **等待時間** | 通常依 2MSL 設定，可能是數十秒到數分鐘 | 簡化為 2 秒，方便教學與測試 |
| **收到重傳 FIN 時** | 仍可再次回 ACK，維持 TIME_WAIT 防護 | 回 ACK，計時器繼續走完 2 秒 |
| **狀態保存** | 通常使用較輕量的 TIME_WAIT 狀態保存必要資訊 | 沿用 `tcp_table` 槽位，標記為 `TCP_TIME_WAIT` |
| **舊封包防護** | 避免舊連線封包污染後續相同 4-Tuple 連線 | 本機 TAP 測試環境中簡化處理 |

---

# 狀態機雙向流轉路徑總覽

![Day24 TCP 主動關閉與被動關閉狀態機](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day24/Day24_3.png)

---

# 程式碼實作細節

### 1. 結構體與函式宣告擴充 (`include/tcp.h`)

```c
enum tcp_state
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    /* 關閉流程新增狀態 */
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

struct tcp_socket
{
    ...
    /* ★ TIME_WAIT 計時器 */
    uint64_t time_wait_start;  // 進入 TIME_WAIT 的時間戳記 (毫秒)
};

/* 主動關閉與 TIME_WAIT 函式宣告 */
int tcp_send_fin(int fd, struct tcp_socket *conn);
int tcp_close(int fd, struct tcp_socket *conn);
void tcp_check_time_wait(void);
```

---

### 2. 實作發送 FIN 與主動關閉入口 (`src/tcp.c`)

```c
int tcp_send_fin(int fd, struct tcp_socket *conn)
{
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 封裝 TCP Header (帶上 FIN 與 ACK)
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq);
    tcp->ack = htonl(conn->ack);
    tcp->data_offset = (5 << 4);
    tcp->window = htons(4096);
    tcp->flags = TCP_FIN | TCP_ACK;

    // 封裝 IPv4 與 Ethernet Header...
    ...
    write(fd, buffer, sizeof(buffer));

    // ★ 關鍵：FIN 消耗 1 個序號！
    conn->seq++;
    printf("[TCP] Sent FIN-ACK: SEQ=%u, ACK=%u (seq advanced to %u)\n", 
           ntohl(tcp->seq), conn->ack, conn->seq);
    return 0;
}

/* ★ 主動關閉入口函式 (Day 25 close() 的底層實作) */
int tcp_close(int fd, struct tcp_socket *conn)
{
    if (!conn || conn->state != TCP_ESTABLISHED) {
        printf("[TCP Active Close] Error: Connection not ESTABLISHED (cannot close)\n");
        return -1;
    }

    printf("\n[TCP Active Close] ★ Initiating Active Close: Sending FIN...\n");
    if (tcp_send_fin(fd, conn) < 0) {
        return -1;
    }

    conn->state = TCP_FIN_WAIT_1;
    printf("[TCP Active Close] State -> FIN_WAIT_1 (Waiting for peer ACK)\n");
    tcp_dump_table();
    return 0;
}
```

---

### 3. 接收端處理：被動關閉、主動關閉與 TIME_WAIT (`src/tcp.c`)

```c
void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    ...
    // 【被動關閉分支】在 ESTABLISHED 狀態收到對端的 FIN
    if (conn->state == TCP_ESTABLISHED) {
        uint32_t ack_num = ntohl(tcp->ack);
        tcp_process_ack(fd, conn, ack_num);

        if (tcp->flags & TCP_FIN) {
            uint32_t received_seq = ntohl(tcp->seq);
            printf("[TCP] FIN Received from Client! (SEQ=%u)\n", received_seq);

            // FIN 消耗 1 號 (相容帶 payload 情況)
            conn->ack = received_seq + payload_len + 1;

            // 第 2 揮：回覆 ACK，進入 CLOSE_WAIT
            conn->state = TCP_CLOSE_WAIT;
            tcp_send_ack(fd, conn);

            // 第 3 揮：Server 送出自身 FIN，進入 LAST_ACK
            conn->state = TCP_LAST_ACK;
            tcp_send_fin(fd, conn);
            return;
        }
        ...
    }

    // 【被動關閉第 4 揮】處於 LAST_ACK 收到最終 ACK
    if (conn->state == TCP_LAST_ACK) {
        if (tcp->flags & TCP_ACK) {
            uint32_t ack_num = ntohl(tcp->ack);
            if (ack_num == conn->seq) {
                conn->state = TCP_CLOSED;
                printf("[TCP] Final ACK Received! (ACK=%u) -> State: CLOSED\n", ack_num);
                tcp_dump_table();
                return;
            }
        }
    }

    // 【主動關閉路徑 1】處於 FIN_WAIT_1 收到確認我方 FIN 的 ACK
    if (conn->state == TCP_FIN_WAIT_1) {
        if (tcp->flags & TCP_ACK) {
            uint32_t ack_num = ntohl(tcp->ack);
            if (ack_num == conn->seq) {
                conn->state = TCP_FIN_WAIT_2;
                printf("[TCP Active Close] State -> FIN_WAIT_2 (Waiting for peer FIN)\n");
                tcp_dump_table();
                return;
            }
        }
    }

    // 【主動關閉路徑 2】處於 FIN_WAIT_2 收到對方的 FIN
    if (conn->state == TCP_FIN_WAIT_2) {
        if (tcp->flags & TCP_FIN) {
            uint32_t received_seq = ntohl(tcp->seq);
            conn->ack = received_seq + payload_len + 1;
            tcp_send_ack(fd, conn);

            conn->state = TCP_TIME_WAIT;
            conn->time_wait_start = get_current_time_ms();
            printf("[TCP Active Close] State -> TIME_WAIT (2-second timer started)\n");
            tcp_dump_table();
            return;
        }
    }

    // 【主動關閉路徑 3】TIME_WAIT 留守期間若收到重傳的 FIN，再度回送 ACK
    if (conn->state == TCP_TIME_WAIT) {
        if (tcp->flags & TCP_FIN) {
            printf("[TCP TIME_WAIT] Retransmitted Peer FIN received -> Resending Final ACK\n");
            tcp_send_ack(fd, conn);
            return;
        }
    }
}
```

---

### 4. TIME_WAIT 2 秒定時器與主迴圈輪詢 (`src/tcp.c` & `src/tap.c`)

```c
/* 在 src/tcp.c 檢查 2 秒倒數 */
#define TCP_TIME_WAIT_MS 2000

void tcp_check_time_wait(void)
{
    uint64_t now = get_current_time_ms();

    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_TIME_WAIT) {
            if (now - tcp_table[i].time_wait_start >= TCP_TIME_WAIT_MS) {
                printf("\n[TCP TIME_WAIT] ★ 2-second Timer Expired -> State: CLOSED (Socket [%02d] released)\n", i);
                tcp_table[i].state = TCP_CLOSED;
                tcp_dump_table();
            }
        }
    }
}

/* 在 src/tap.c 主迴圈中每 100ms 喚醒檢查 */
while (1) {
    ...
    int sel = select(fd + 1, &fds, NULL, NULL, &tv); // 100ms 超時
    tcp_check_time_wait();       // ★ 定期檢查 TIME_WAIT 倒數
    tcp_check_retransmission(fd); // ★ 定期檢查超時重傳
    if (sel <= 0) continue;
    ...
}
```

---

# 實測驗證與真實日誌

### 測試一：Server 被動關閉驗證 (`send_tcp_fin`)

#### Client 側真實輸出：
```text
=== [Phase 1: 建立連線 (Handshake)] ===
[Handshake] 發送 TCP SYN (SEQ=1000)...
[Handshake] 收到 Server SYN-ACK (Server SEQ=5000, ACK=1001)
[Handshake] 發送交握 ACK (SEQ=1001, ACK=5001) -> 連線 ESTABLISHED！

=== [Phase 2: TCP Four-Way Teardown (連線關閉)] ===
[Wave 1/4] Client 發送 FIN-ACK (SEQ=1001, ACK=5001) -> 狀態: FIN_WAIT_1
[Wave 2/4] ★ 收到 Server 回覆 ACK (ACK=1002) -> Client 進入 FIN_WAIT_2
[Wave 3/4] ★ 收到 Server 發送 FIN-ACK (Server SEQ=5001) -> Server 進入 LAST_ACK
[Wave 4/4] Client 發送最終 ACK (SEQ=1002, ACK=5002) -> 連線正式 CLOSED！

🎉 TCP 四向揮手 (Four-Way Teardown) 測試圓滿成功！
```

#### Server 側真實狀態流轉與 Socket Table：
```text
========================================
[TCP] FIN Received from Client! (SEQ=1001)
[TCP] Sending ACK for FIN -> State: CLOSE_WAIT
[TCP] Sent ACK: SEQ=5001, ACK=1002
[TCP] Server closing -> Sending FIN -> State: LAST_ACK
[TCP] Sent FIN-ACK: SEQ=5001, ACK=1002 (seq advanced to 5002)
========================================

... (Client 發送最終 ACK: SEQ=1002, ACK=5002) ...

========================================
[TCP] Final ACK Received!
[TCP] Four-Way Teardown Complete: State -> CLOSED
========================================

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
========================
```

---

### 測試二：Server 主動關閉與 `TIME_WAIT` 2 秒倒數釋放 (`send_tcp_active_close`)

這是最精采的一幕！Client 發送帶有 `Connection: CLOSE` 的請求，觸發 Server 主動關閉並留守 `TIME_WAIT`：

#### Client 側真實輸出：
```text
=== [Step 1: 三向交握建立 ESTABLISHED] ===
[Handshake] Client 發送 SYN (SEQ=1000)...
[Handshake] 收到 Server SYN-ACK (Server SEQ=5000, ACK=1001)
[Handshake] 發送交握 ACK (SEQ=1001, ACK=5001) -> 連線 ESTABLISHED！

=== [Step 2: Client 發送含 CLOSE 的請求，觸發 Server 主動關閉] ===
[Request] 發送帶有 'Connection: CLOSE' 的請求...

=== [Step 3: 等待 Server 回傳資料與 Server 主動送出的第 1 揮 (FIN)] ===
[Response] 收到 Server 回傳資料：24 bytes
[Wave 1/4] ★ 收到 Server 主動送出的 FIN！(Server SEQ=5025) -> Server 進入 FIN_WAIT_1

=== [Step 4: Client 回覆 ACK，推動 Server 進入 FIN_WAIT_2] ===
[Wave 2/4] Client 送出 ACK (ACK=5026) -> Server 進入 FIN_WAIT_2！

=== [Step 5: Client 送出 FIN，推動 Server 進入 TIME_WAIT] ===
[Wave 3/4] Client 送出 FIN-ACK (SEQ=1038) -> 等待 Server 回覆最後 ACK 並進入 TIME_WAIT...
[Wave 4/4] ★ 成功收到 Server 最終 ACK (ACK=1039)！
           此時 Server 已正式進入 TIME_WAIT 狀態，啟動 2 秒倒數！

=== [Step 6: 觀察 TIME_WAIT 到期釋放] ===
[Waiting] Client 停留 3 秒... 請觀察 Server 終端機在 2 秒後是否自動轉為 CLOSED！    
  ...等待中 (1 秒)
  ...等待中 (2 秒)
  ...等待中 (3 秒)

🎉 Server 主動關閉與 TIME_WAIT 驗證流程全部結束！
```

#### Server 側真實狀態流轉與 Socket Table：
```text
[TCP] Received Payload: SEQ=1001, Len=37 (Expected SEQ=1001)
[TCP Reassembly] Packet In-Order! Delivering to application...

[TCP DATA]
GET / HTTP/1.1
Connection: CLOSE

[TCP Send Buffer] Saved segment in slot [00]: SEQ=5001, Len=24
[TCP] Sent Data: 24 bytes | New SEQ=5025, ACK=1001

[TCP Active Close] ★ Initiating Active Close: Sending FIN...
[TCP] Sent FIN-ACK: SEQ=5025, ACK=1001 (seq advanced to 5026)
[TCP Active Close] State -> FIN_WAIT_1 (Waiting for peer ACK)

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
[01] State: FIN_WAIT_1   | 10.0.0.1:52144 -> 10.0.0.2:8080
========================

========================================
[TCP Active Close] ACK for our FIN received! (ACK=5026)
[TCP Active Close] State -> FIN_WAIT_2 (Waiting for peer FIN)
========================================

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
[01] State: FIN_WAIT_2   | 10.0.0.1:52144 -> 10.0.0.2:8080
========================

========================================
[TCP Active Close] Peer FIN received! (SEQ=1038)
[TCP Active Close] Sending Final ACK -> State: TIME_WAIT
[TCP] Sent ACK: SEQ=5026, ACK=1039
[TCP Active Close] State -> TIME_WAIT (2-second timer started)
========================================

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
[01] State: TIME_WAIT    | 10.0.0.1:52144 -> 10.0.0.2:8080
========================

... (靜待 2 秒後，主迴圈 select 喚醒 tcp_check_time_wait) ...

[TCP TIME_WAIT] ★ 2-second Timer Expired -> State: CLOSED (Socket [01] released)    

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
========================
```

親眼看著 Socket Table 中的 `[01]` 槽位從 `FIN_WAIT_1` $\rightarrow$ `FIN_WAIT_2` $\rightarrow$ `TIME_WAIT`，最後在 2 秒倒數完畢後自動釋放，只留下乾淨的 `LISTEN` socket。

---

# 總結與目前 TCP 協定棧全貌

至此，我們的教學版 TCP Stack 已經走完 TCP 連線生命週期中最重要的主線：建立連線、資料傳輸、可靠性處理與關閉流程。

```text
自製 TCP/IP 協定棧目前成果：
✓ Layer 2: Ethernet Frame 封裝與解析
✓ Layer 2.5: ARP Request、Reply 與動態 ARP Table
✓ Layer 3: IPv4 封裝、Checksum 計算與本機路由分流
✓ Layer 3: ICMP Echo Request / Reply (Ping)
✓ Layer 4: UDP Receiver、Sender 與 DNS Client 查詢
✓ Layer 4: TCP Three-Way Handshake (三向交握連線建立)
✓ Layer 4: TCP Sequence Number & Cumulative ACK 序號管理
✓ Layer 4: TCP Out-of-Order Reassembly (亂序暫存與連續重組)
✓ Layer 4: TCP Duplicate ACK & Fast Retransmit (快速重傳)
✓ Layer 4: TCP Retransmission Timeout (1000ms RTO 超時重傳)
✓ Layer 4: TCP Passive Close (被動關閉: CLOSE_WAIT -> LAST_ACK -> CLOSED)
✓ Layer 4: TCP Active Close (主動關閉: FIN_WAIT_1 -> FIN_WAIT_2 -> TIME_WAIT)
✓ Layer 4: TCP TIME_WAIT Timer (2 秒防禦留守與自動超時釋放)
```

---

# Day 25 預告：POSIX Socket API 封裝

雖然我們的 TCP Stack 已經具備基本的連線生命週期，但目前的程式碼呼叫依然是底層的內部函式（如 `tcp_find_connection`、`tcp_send`、`tcp_close`）。

明天（Day 25），我們將迎來 TCP 章節的最終章：**POSIX Socket API 封裝**！
我們將把這些底層細節包裝成所有軟體工程師最熟悉的標準介面：
```c
socket()
bind()
listen()
accept()
recv()
send()
close()
```
完成後，上層應用程式就能用更接近 POSIX Socket 的方式呼叫我們的教學版 TCP Stack，而不必直接操作底層 TCP 函式。
