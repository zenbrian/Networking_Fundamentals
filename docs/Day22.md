# Day 22：TCP Reassembly — 用 Sequence Number 重組資料流

在 TCP 通訊中，底層 IP 網路為不可靠的封包交換網路，不保證封包能依照發送順序抵達接收端。若直接將接收到的封包依序交給應用程式，將會導致資料交錯亂序（例如將 `"Hello World"` 錯印為 `"WorldHello"`）。

今天（Day 22），我們實作 TCP 重組機制（Reassembly Buffer）與累積確認（Cumulative ACK）：當收到未來的亂序封包時，先將它暫存起來；等缺少的 Sequence Number 封包到達後，再自動連續釋放已暫存的後續片段，還原出正確的位元組流（Byte Stream）。

![Day22 TCP 亂序暫存與重組流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day22/Day22_1.png)

---

# 今日學習目標與成果

- [x] 理解 TCP 為什麼需要依 Sequence Number 重組 Byte Stream。
- [x] 在 `tcp_socket` 中加入 `expected_seq`，追蹤下一個期待收到的序號。
- [x] 建立 `tcp_fragment` 暫存亂序 Payload。
- [x] 實作 `store_fragment()`，保存未來才會用到的封包。
- [x] 實作 `process_buffered_fragments()`，在缺口補齊後連續釋放暫存資料。
- [x] 在 `tcp_receive()` 中分辨 In-Order、Out-of-Order 與 Duplicate Payload。
- [x] 使用 Cumulative ACK 回報目前已連續收到的 byte 邊界。
- [x] 測試先收到 `World\n`、再收到 `Hello `，成功重組為 `Hello World\n`。

---

# 核心概念深入剖析

### 1. TCP Byte Stream 與 Sequence Number 的本質

UDP 屬於 Datagram（報文）導向的協定，每個封包彼此獨立；而 TCP 提供的是 **Byte Stream（位元組流）** 服務。應用程式關心的是完整的連續資料，而非單個網卡封包。

```text
發送端位元組流： [H][e][l][l][o][ ][W][o][r][l][d][\n]
                 ▲              ▲
                 SEQ=1001       SEQ=1007
```

TCP 透過 **Sequence Number** 為每一個傳輸的 Byte 編號。接收端透過 `expected_seq` 追蹤「目前已連續接收的邊界」，確保應用程式永遠看到正確排序的資料流。

---

### 2. TCP Payload 三大處理分流

下圖展示了在 `conn->state == TCP_ESTABLISHED` 收到 Payload 時，系統根據 `received_seq` 與 `conn->expected_seq` 比對後的處理流程：

![Day22 TCP ESTABLISHED Payload 分流流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day22/Day22_2.png)

| 分流情況 | 比對條件 | 系統處置動作 | Expected SEQ 推進 |
| :--- | :--- | :--- | :--- |
| **In-Order (按順序)** | `received_seq == expected_seq` | 1. 交付 Payload 給應用程式<br>2. 推進 `expected_seq += len`<br>3. 呼叫 `process_buffered_fragments()` | 推進 `+ len` (及暫存包長度) |
| **Out-of-Order (亂序)** | `received_seq > expected_seq` | 1. 偵測中間缺漏資料<br>2. 呼叫 `store_fragment()` 暫存到 Reassembly Buffer | **不推進** |
| **Duplicate (重複)** | `received_seq < expected_seq` | 1. 辨識為過去已接收過的重複封包<br>2. 忽略 Payload | **不推進** |

---

### 3. Cumulative ACK（累積確認）的精髓

> **教學版限制**：本日以固定 32 格陣列暫存亂序片段，尚未處理片段重疊、片段合併、Reassembly Buffer 滿載、記憶體壓力回收與惡意封包防護。

TCP 採用 **Cumulative ACK** 機制。接收端回覆的 `ACK` 號碼代表：
> **「我已經完整且連續收到了 Sequence Number 比 ACK 小的所有位元組，下一個請傳送序號等於 ACK 的位元組給我。」**

例如：
1. 先收到 `SEQ=1007` (Len=6) $\rightarrow$ 但缺了 `1001~1006` $\rightarrow$ Server 回覆 `ACK=1001`（告訴 Client: 我還在等 1001！）。
2. 後收到 `SEQ=1001` (Len=6) $\rightarrow$ 補齊 1001~1006 且連鎖取出 1007~1012 $\rightarrow$ Server 回覆 `ACK=1013`（告訴 Client: 我已經連續收到 1012 了，下一個請傳 1013！）。

---

# 程式碼實作細節

### 1. 資料結構擴充 (`include/tcp.h`)

```c
/* TCP 亂序封包暫存結構 */
struct tcp_fragment
{
    uint32_t seq;       // 亂序封包起點序號
    uint16_t len;       // Payload 長度
    uint8_t data[1500]; // 暫存資料內容
    int used;           // 0: 空位, 1: 已佔用
};

struct tcp_socket
{
    ...
    uint32_t expected_seq;     // 我下一個期待收到的 Client Sequence Number
    enum tcp_state state;

    /* 亂序重組 Buffer */
    struct tcp_fragment fragments[32];
    int fragment_count;
};
```

---

### 2. 核心重組與分流邏輯 (`src/tcp.c`)

```c
/* 儲存亂序 (未來) 封包至 Buffer */
static void store_fragment(struct tcp_socket *conn, uint32_t seq, const uint8_t *data, uint16_t len)
{
    for (int i = 0; i < 32; i++) {
        if (conn->fragments[i].used && conn->fragments[i].seq == seq) {
            return; // 避免重複暫存
        }
    }

    for (int i = 0; i < 32; i++) {
        if (!conn->fragments[i].used) {
            conn->fragments[i].seq = seq;
            conn->fragments[i].len = len;
            memcpy(conn->fragments[i].data, data, len);
            conn->fragments[i].used = 1;
            conn->fragment_count++;
            printf("[TCP Buffer] Stored Out-of-Order Fragment: SEQ=%u, Len=%u\n", seq, len);
            return;
        }
    }
}

/* 檢查並重組 Buffer 中的連續封包 */
static void process_buffered_fragments(struct tcp_socket *conn)
{
    int found = 1;
    while (found) {
        found = 0;
        for (int i = 0; i < 32; i++) {
            if (conn->fragments[i].used && conn->fragments[i].seq == conn->expected_seq) {
                printf("\n[TCP Reassembly] Found matching buffered fragment! SEQ=%u, Len=%u\n",
                       conn->fragments[i].seq, conn->fragments[i].len);
                
                tcp_dump_payload(conn->fragments[i].data, conn->fragments[i].len);
                conn->expected_seq += conn->fragments[i].len;
                conn->fragments[i].used = 0;
                conn->fragment_count--;

                found = 1; // 繼續檢查是否有連續的下一個封包
                break;
            }
        }
    }
}
```

---

# 實務驗證 log 紀錄

在測試程式 `send_tcp_reassembly` 刻意先送出 `SEQ=1007 ("World\n")` 後送出 `SEQ=1001 ("Hello ")` 時的完整執行日誌：

```text
[TCP] Received Payload: SEQ=1007, Len=6 (Expected SEQ=1001)
[TCP Reassembly] Out-of-Order Packet detected! (Missing bytes before SEQ 1007)
[TCP Buffer] Stored Out-of-Order Fragment: SEQ=1007, Len=6 (Total Buffered: 1)
[TCP] Sent ACK: SEQ=5001, ACK=1001

[TCP] Received Payload: SEQ=1001, Len=6 (Expected SEQ=1001)
[TCP Reassembly] Packet In-Order! Delivering to application...

[TCP DATA]
Hello 

[TCP Reassembly] Found matching buffered fragment! SEQ=1007, Len=6

[TCP DATA]
World

[TCP] Sent ACK: SEQ=5001, ACK=1013
```

---

# 總結與 Day 23 預告

今天我們成功解決了 TCP 中的 **亂序 (Out-of-Order)** 與 **重複 (Duplicate)** 兩大挑戰，使 TCP Stack 具備了強大的 Byte Stream 重組能力。

明天（Day 23），我們將挑戰 TCP 可靠傳輸（Reliable Transmission）的另一個核心痛點：**封包遺失 (Packet Loss) 與重傳機制 (Retransmission / Fast Retransmit)**！
