# Day 23：TCP Retransmission — 封包遺失與重傳機制

在不可靠的 IP 網路中，除了封包「亂序」抵達外，更嚴重的問題是**「封包遺失（Packet Loss）」**。
若發送端只是將資料發出就不管，一旦封包遺失，整個連線的資料流就會出現永久性的斷層。

今天（Day 23），我們在自製的 TCP 協定棧中實作了 TCP 最具代表性的核心可靠傳輸機制：**重傳緩衝區（Send Buffer）**、**重複確認偵測（Duplicate ACK）**、**快速重傳（Fast Retransmit）** 與 **超時重傳計時器（Retransmission Timeout, RTO）**。

![Day23 TCP 封包遺失與重傳判斷流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day23/Day23_1.png)

---

# 今日學習目標與成果

- [x] 建立 `send_buffer`，保存已送出但尚未被 ACK 的資料。
- [x] 在 `tcp_send()` 發送資料時，記錄 `seq`、`len`、payload 與送出時間。
- [x] 收到新的 ACK 時，釋放已被累積確認的 buffer slot。
- [x] 使用 `last_ack` 與 `dup_ack_count` 偵測 Duplicate ACK。
- [x] 收到 3 個 Duplicate ACK 時觸發 Fast Retransmit。
- [x] 實作 RTO 超時檢查，1 秒未被 ACK 就重傳。
- [x] 重傳時維持原始 SEQ，不推進 `conn->seq`。
- [x] 使用 `select()` 讓主迴圈能定期檢查重傳計時器。
- [x] 使用 `send_tcp_retransmit` 驗證 Fast Retransmit、RTO 與 Send Buffer 釋放。

---

# 核心概念深入剖析

### 1. ACK 的真正意義：累積確認（Cumulative ACK）

在 TCP 中，`ACK=X` 並不表示「我收到了第 X 號封包」，而是代表：
> **「X 號以前的資料我都完整收到了，請傳送序號 X 給我。」**

假設 Client 期待 Server 資料：
```text
Server 送出：SEQ=5001 (Len=24)
正常收到後：Client 回覆 ACK=5025 (5001 + 24)
```

如果該封包遺失，接收端若收到後續的封包，因為中間有洞，接收端**不能更新 ACK**，必須持續回傳相同的 ACK 號碼。

---

### 2. Duplicate ACK 與 Fast Retransmit（快速重傳）

當網路發生單一封包遺失，但後續封包仍陸續抵達時：
1. 接收端每收到一個後續封包，就必須回覆一次目前的期望序號（例如 `ACK=5001`）。
2. 發送端收到第 1 次 `ACK=5001`：屬正常情況。
3. 發送端收到第 2 次 `ACK=5001`：可能是網路封包稍微延遲或微亂序。
4. 發送端收到第 3 次 `ACK=5001`：此時**高度肯定 `5001` 已經在網路中遺失**！

本教學版採用常見簡化規則：連續觀察到 3 次相同 ACK，就觸發 Fast Retransmit。

**Fast Retransmit 的好處**：
傳統超時重傳需要等待 RTO（例如 1000ms），會造成整個傳輸暫停；而快速重傳只要湊滿 3 個 Duplicate ACK，便可在數毫秒內**立即補發遺失封包**，大幅提升網路輸送量。

```text
發送端 (Server)                     接收端 (Client)
      |                                    |
      | -------- SEQ=5001 (遺失) --------> X
      | -------- SEQ=5025 ---------------> | (收到未來的資料，缺 5001)
      | <------- ACK=5001 (Dup #1) ------- |
      | -------- SEQ=5045 ---------------> |
      | <------- ACK=5001 (Dup #2) ------- |
      | -------- SEQ=5065 ---------------> |
      | <------- ACK=5001 (Dup #3) ------- |
      |                                    |
[觸發 Fast Retransmit!]                     |
      | ======= 重傳 SEQ=5001 ===========> | (補齊缺漏)
      | <======= ACK=5085 ================ | (連續累積確認)
```

---

### 3. Retransmission Timeout（RTO 超時重傳）

若整條連線中最後一個封包掉了，或者整批封包都掉了，接收端根本收不到後續資料，就**無法產生 Duplicate ACK**。
此時發送端必須仰賴**超時重傳計時器（RTO）**：

```text
now - send_time >= 1000 ms 且 acked == 0
```
發送端掃描緩衝區發現超時，便主動再次送出原始段落，並刷新 `send_time`。

---

# 程式碼實作細節

### 1. 資料結構擴充 (`include/tcp.h`)

```c
/* TCP 傳送段落緩衝結構 (用於超時重傳與快速重傳) */
struct tcp_segment
{
    uint32_t seq;           // 段落起始序號
    uint16_t len;           // Payload 長度
    uint8_t data[1500];     // 段落資料備份
    uint64_t send_time;     // 發送時間戳記 (毫秒)
    int acked;              // 0: 未確認, 1: 已確認
    int used;               // 0: 空槽位, 1: 已佔用
    int retransmit_count;   // 已重傳次數
};

struct tcp_socket
{
    ...
    /* 重傳機制相關欄位 */
    struct tcp_segment send_buffer[64]; // 發送緩衝區
    uint32_t last_ack;                  // 上一次收到的 ACK 號碼
    int dup_ack_count;                  // 重複 ACK 計數器
};
```

---

### 2. 發送資料時存入緩衝區 (`src/tcp.c`)

在 `tcp_send()` 發送資料時，將封包備份至 `send_buffer`：

```c
for (int i = 0; i < 64; i++) {
    if (!conn->send_buffer[i].used) {
        conn->send_buffer[i].seq = conn->seq;
        conn->send_buffer[i].len = len;
        memcpy(conn->send_buffer[i].data, data, len);
        conn->send_buffer[i].send_time = get_current_time_ms();
        conn->send_buffer[i].acked = 0;
        conn->send_buffer[i].used = 1;
        conn->send_buffer[i].retransmit_count = 0;
        printf("[TCP Send Buffer] Saved segment in slot [%02d]: SEQ=%u, Len=%zu\n",
               i, conn->seq, len);
        break;
    }
}
conn->seq += len;
```

---

### 3. ACK 處理與快速重傳判斷 (`src/tcp.c`)

```c
static void tcp_process_ack(int fd, struct tcp_socket *conn, uint32_t ack_num)
{
    if (ack_num > conn->last_ack) {
        // 情況 A：收到新 ACK (推進)
        conn->last_ack = ack_num;
        conn->dup_ack_count = 0;

        // 釋放已確認收到的段落
        for (int i = 0; i < 64; i++) {
            if (conn->send_buffer[i].used) {
                if (conn->send_buffer[i].seq + conn->send_buffer[i].len <= ack_num) {
                    printf("[TCP Send Buffer] Segment SEQ=%u..%u acknowledged -> Free slot [%02d]\n",
                           conn->send_buffer[i].seq,
                           conn->send_buffer[i].seq + conn->send_buffer[i].len,
                           i);
                    conn->send_buffer[i].acked = 1;
                    conn->send_buffer[i].used = 0;
                }
            }
        }
    } else if (ack_num == conn->last_ack && conn->last_ack > 0) {
        // 情況 B：收到重複 ACK
        conn->dup_ack_count++;
        printf("[TCP ACK] Duplicate ACK detected: %u (Count = %d)\n", ack_num, conn->dup_ack_count);

        // 3 個 Duplicate ACK 觸發快速重傳
        if (conn->dup_ack_count == 3) {
            printf("\n[TCP Fast Retransmit] ★ 3 Duplicate ACKs received! Fast Retransmitting SEQ=%u...\n", ack_num);
            for (int i = 0; i < 64; i++) {
                if (conn->send_buffer[i].used && conn->send_buffer[i].seq == ack_num) {
                    tcp_resend_segment(fd, conn, &conn->send_buffer[i]);
                    break;
                }
            }
        }
    }
}
```

---

### 4. 超時檢查與事件迴圈驅動 (`src/tap.c` 與 `src/tcp.c`)

在 `src/tap.c` 主迴圈中使用 `select()` 實現非阻塞定時輪詢：

```c
while (1) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 }; // 100ms 超時
    int sel = select(fd + 1, &fds, NULL, NULL, &tv);

    // 每隔 100ms 或每次有封包進出時，檢查一次超時重傳
    tcp_check_retransmission(fd);
    if (sel <= 0) {
        continue;
    }
    int n = read(fd, buffer, sizeof(buffer));
    ...
}
```

在 `tcp_check_retransmission()` 中檢查超過 1000ms 的封包並重新發送：

```c
void tcp_check_retransmission(int fd)
{
    uint64_t now = get_current_time_ms();
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_ESTABLISHED) {
            for (int j = 0; j < 64; j++) {
                if (tcp_table[i].send_buffer[j].used && !tcp_table[i].send_buffer[j].acked) {
                    if (now - tcp_table[i].send_buffer[j].send_time >= 1000) {
                        if (tcp_table[i].send_buffer[j].retransmit_count >= 5) {
                            tcp_table[i].send_buffer[j].used = 0;
                            continue;
                        }
                        tcp_table[i].send_buffer[j].retransmit_count++;
                        printf("\n[TCP Timeout Retransmit] ★ Segment SEQ=%u timeout (%llu ms)! Retransmitting...\n",
                               tcp_table[i].send_buffer[j].seq,
                               (unsigned long long)(now - tcp_table[i].send_buffer[j].send_time));
                        tcp_resend_segment(fd, &tcp_table[i], &tcp_table[i].send_buffer[j]);
                    }
                }
            }
        }
    }
}
```

---

# 實測驗證與日誌流程

![Day23 send_tcp_retransmit 核心重傳與釋放測試情境](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day23/Day23_2.png)

### 測試執行方式
在 WSL 開啟兩個終端機：
* **Terminal 1（協定棧本體）**：`sudo ./network`
* **Terminal 2（重傳測試器）**：`sudo ./send_tcp_retransmit`

---

### 端對端測試五大步驟輸出

```text
[Step 1] 發送 TCP SYN (SEQ=1000)...
[Step 1] 收到 SYN-ACK (Server SEQ=5000, ACK=1001)
[Step 1] 發送交握 ACK (SEQ=1001, ACK=5001) -> 連線 ESTABLISHED！

[Step 2] 發送 Client 請求觸發 Server 回應...
[Step 2] 成功收到 Server 資料！(SEQ=5001, Len=24)
         Server 端記錄：[TCP Send Buffer] Saved segment in slot [00]: SEQ=5001, Len=24

[Step 3] 故意送出 3 個 Duplicate ACK (ACK=5001) 測試快速重傳...
  -> 發送 Duplicate ACK #1 (ACK=5001)
  -> 發送 Duplicate ACK #2 (ACK=5001)
  -> 發送 Duplicate ACK #3 (ACK=5001)
[Step 3] 等待 Server Fast Retransmit 重傳封包...
Server 觸發：[TCP Fast Retransmit] ★ 3 Duplicate ACKs received! Fast Retransmitting SEQ=5001...
[Step 3] 成功收到 Server 快速重傳的封包 (SEQ=5001, Len=24)！

[Step 4] 故意靜默不回覆 ACK，等待 1 秒 RTO 超時重傳...
Server 觸發：[TCP Timeout Retransmit] ★ Segment SEQ=5001 timeout (1001 ms, Retry #1)! Retransmitting...
[Step 4] 成功收到 Server 超時重傳的封包 (SEQ=5001, Len=24)！

[Step 5] 發送最終 ACK (ACK=5025) 確認收訖，測試 Server 釋放 Send Buffer...
Server 觸發：[TCP Send Buffer] Segment SEQ=5001..5025 acknowledged -> Free slot [00]
全部測試完成！
```

---

# 總結與目前 TCP 協定棧完整度

截至 Day 23，我們的教學版 TCP Stack 已經具備可靠傳輸的基本骨架：

```text
TCP 功能清單：
✓ Three-Way Handshake (三向交握建立連線)
✓ Sequence Number & ACK 同步
✓ Cumulative ACK (累積確認)
✓ Out-of-Order Reassembly (亂序重組與 Reassembly Buffer)
✓ Duplicate ACK 偵測
✓ Fast Retransmit (3 個重複確認快速重傳)
✓ Retransmission Timer (1 秒 RTO 超時重傳)
✓ Send Buffer 生命週期管理 (自動釋放已確認段落)
```

---

# Day 24 預告：TCP Connection Close (Four-Way Teardown)

目前我們的 TCP 連線建立後能可靠收發資料，但永遠處於 `ESTABLISHED` 狀態。
下一天（Day 24），我們將實作 **TCP 連線終止機制**：
```text
Client 送出 FIN  ───>  Server 回覆 ACK
Server 送出 FIN  ───>  Client 回覆 ACK
```
實作完整的 TCP 四向揮手（Four-Way Teardown）與狀態機轉移（`FIN_WAIT_1`, `CLOSE_WAIT`, `LAST_ACK`, `CLOSED`），為整個 TCP 核心章節劃下完美句點！
