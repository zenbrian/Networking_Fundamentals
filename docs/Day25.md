# Day 25：Socket API 封裝 — 用類 POSIX 介面操作自製 TCP Stack

經過前 24 天的努力，我們從最底層的虛擬網卡 TAP 開始，一路手刻完成了 Ethernet、ARP、IPv4、ICMP、UDP，以及 TCP 的教學版核心主線：三向交握（Three-Way Handshake）、SEQ/ACK、亂序重組（Reassembly）、重傳（Retransmission）以及四向揮手終止機制（Four-Way Teardown）。

但在今天之前，如果你想寫一個 Echo Server，程式碼長得非常痛苦：
```c
// 過去：應用層被迫與底層網卡硬體綁死
tcp_init();
tcp_listen(8080);
while (1) {
    read(fd, buffer, sizeof(buffer)); // 自己讀網卡
    // 自己解 Ethernet -> IPv4 -> TCP ...
}
```

真正的 Linux 網路應用程式（如 Nginx、Node.js、Redis）不會直接操作網卡封包，而是透過經典的 **Berkeley Socket API**：
```c
sock = socket();
bind(sock, 8080);
listen(sock);
conn = accept(sock);
recv(conn, buf, ...);
send(conn, buf, ...);
close(conn);
```

本專案並不是直接實作 Linux 系統呼叫，而是設計一組**類 POSIX 的教學版 Socket API**；名稱與參數較簡化，但抽象概念相同。

今天（Day 25），我們的目標就是打造一套**類 POSIX 的教學版 Socket 抽象層**，將複雜的網路協定棧細節隔離起來；同時補上 TCP Pseudo Header Checksum，讓 Linux 原生工具（如 `nc`）能接受我們送出的 TCP 封包，進一步驗證自製 TCP Stack 的基本互通性。

---

# 今日學習目標與成果

- [x] 理解 Socket API 如何隔離應用程式與底層 TCP/IP 細節。
- [x] 設計 `struct socket`，作為應用層持有的連線 Handle。
- [x] 實作 `socket_create()` 與 `socket_bind()`。
- [x] 實作 `socket_listen()`，串接底層 TCP listener。
- [x] 理解 Listener Socket 與 Connection Socket 的差異。
- [x] 實作 `socket_accept()`，把已建立的 TCP connection 包裝成 socket。
- [x] 實作 `socket_recv()`，從 TCP receive buffer 讀取資料。
- [x] 實作 `socket_send()`，透過底層 `tcp_send()` 發送資料。
- [x] 實作 `socket_close()`，觸發 TCP 主動關閉流程。
- [x] 補上 TCP Pseudo Header Checksum，讓 Linux `nc` 能接受封包。
- [x] 使用 Echo Server 驗證 Socket API 的基本流程。

---

# 核心概念深入剖析

### 1. Socket 具體來說是什麼？

在網路世界裡，**Socket（插座 / 話筒）是應用層與作業系統核心協定棧之間的「標準溝通界面」**。

在作業系統記憶體中，它本質上就是一個資料結構（`struct`），主要維護四件事：
1. **通訊端點（4-Tuple）**：本地 IP:Port + 遠端 IP:Port。
2. **連線狀態（State Machine）**：目前是等電話（LISTEN）、通話中（ESTABLISHED）還是已掛斷（CLOSED）。
3. **收發雙向郵箱（Buffers）**：
   - **Send Buffer**：應用程式要送出的字串，暫存等待被切成封包送出。
   - **Receive Buffer**：網卡收到、確認 Checksum 並依序重組好的 Payload，暫存等待應用程式讀取。
4. **號碼牌（Handle / File Descriptor）**：供應用層持有的不透明代號。

![Day25 Socket API 抽象層架構圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day25/Day25_1.png)

---

### 2. Listener Socket vs Connection Socket（接待生 vs 專線電話）

這是網路程式設計中最容易被混淆的概念：

| 維度 | Listener Socket (監聽端點) | Connection Socket (連線端點) |
| :--- | :--- | :--- |
| **產生時機** | 伺服器啟動時由 `socket_create()` + `listen()` 建立 | 三向交握成功後由 `socket_accept()` 建立 |
| **生命週期** | 伺服器運作期間通常長時間存在 | 一對一服務單一客戶端，客人離開即 `close()` 釋放 |
| **底層狀態** | `TCP_LISTEN`（如 `:8080`） | `TCP_ESTABLISHED`（如 `10.0.0.1:58526 -> 10.0.0.2:8080`） |
| **主要功能** | 像**餐廳接待生**，專門站在門口聽客人的 `SYN` 請求 | 像**餐桌專線**，專門進行資料傳輸（`recv` / `send`） |

因此，`socket_accept()` 的任務就是：**去底層尋找「已經完成三向交握、且尚未被認領的 Connection」，為其包裝一個新的 `struct socket` 返回給應用程式！**

---

### 3. 接收緩衝區（Receive Buffer）與排隊向前靠攏

當網卡收到 TCP Payload 時，應用程式可能還在處理其他事情、尚未呼叫 `recv()`。因此資料必須先暫存在 `conn->recv_buf`。

當應用程式呼叫 `socket_recv(conn, buf, len)` 讀取資料時，如果它只讀了其中一部分（例如郵箱有 11 Bytes，但只讀了 5 Bytes），剩餘的 6 Bytes 必須透過 `memmove` **「往前靠攏」**：

![Day25 Receive Buffer 滑移圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day25/Day25_2.png)

**效益**：永遠確保 `recv_buf[0]` 是下一次讀取的起始點，新抵達的封包永遠接在 `recv_buf + recv_len` 後面，實現乾淨的 FIFO 佇列。

在本教學版中，當 receive buffer 已空，且底層 TCP 進入 `TCP_CLOSE_WAIT` 或 `TCP_CLOSED` 時，`socket_recv()` 會回傳 `0`，表示應用層已讀到 EOF。

> **教學版限制**：本日使用固定大小 `recv_buf[8192]`，尚未實作動態擴充、背壓控制、receive window 更新與零視窗處理。

---

### 4. 協定棧心跳：`net_poll()`

在單執行緒的使用者空間協定棧中，當應用程式在 `socket_accept()` 等待連線、或在 `socket_recv()` 等待資料時，誰來讀取網卡？

👉 **答案就是 `net_poll()`！**
- 它執行單次事件檢查：`select` 100ms 等待網卡是否可讀。
- 檢查逾時重傳與 `TIME_WAIT` 計時器。
- 若有封包進入，處理該單一訊框並分流至 ARP / IPv4 / TCP，然後**立刻 return**！
- 讓 `socket_accept` 和 `socket_recv` 能夠在迴圈中不斷推進底層狀態。

---

# 核心資料結構與 API 實作

### 1. 結構定義與狀態列舉（`include/socket.h`）

```c
enum socket_state {
    SOCKET_UNUSED = 0,  // 空位，尚未分配
    SOCKET_CLOSED,      // 已分配，尚未連線或已被關閉
    SOCKET_LISTEN,      // 正在監聽中 (Listener)
    SOCKET_ESTABLISHED, // 連線已建立 (Connection)
};

struct socket {
    int id;                  // Handle 編號 (類似 Linux fd)
    uint16_t port;           // 綁定的通訊埠
    int state;               // Socket 狀態
    struct tcp_socket *tcp;  // 關聯到底層 TCP 狀態機結構
};
```

### 2. 接收緩衝區擴充（`include/tcp.h`）

```c
struct tcp_socket {
    ...
    /* ★ 接收緩衝區 (Receive Buffer) */
    uint8_t recv_buf[8192];
    size_t recv_len;
};
```

### 3. 七大 Socket API 實作（`src/socket.c`）

```c
// 1. 分配 Socket
struct socket* socket_create(void);

// 2. 綁定通訊埠
int socket_bind(struct socket *sock, uint16_t port);

// 3. 啟動監聽模式
int socket_listen(struct socket *sock);

// 4. 等待並接受新連線
struct socket* socket_accept(struct socket *listener);

// 5. 接收資料 (支援 EOF 斷線判斷與緩衝區滑移)
int socket_recv(struct socket *sock, uint8_t *buf, size_t len);

// 6. 發送資料 (封裝 TCP Header 並計算 Checksum)
int socket_send(struct socket *sock, const uint8_t *buf, size_t len);

// 7. 優雅關閉連線 (觸發四向揮手送出 FIN)
int socket_close(struct socket *sock);
```

`socket_close()` 會從應用層角度關閉 handle，並交由底層 TCP 狀態機繼續完成 `FIN_WAIT_1`、`FIN_WAIT_2` 與 `TIME_WAIT` 流程。

### 4. TCP Pseudo Header Checksum 實作（`src/checksum.c`）

在測試真實 Linux 原生 `nc` 工具時，我們遇到了經典的阻礙：**連線卡在 `SYN_RECEIVED`，客戶端持續重傳 SYN！**

這是因為：
- 在 IPv4 中，UDP Checksum 可以設為 0 表示「不校驗」。
- **TCP Checksum 則是必要欄位。** 若發送的 `SYN-ACK` Checksum 為 0，Linux 核心會直接視為損毀封包丟棄。

TCP 校驗和必須將 **12 位元組的虛擬標頭（Pseudo Header）** 與 TCP 標頭及 Payload 合併計算：

```text
+------------------------+------------------------+
|               Source IP (4 Bytes)               |
+------------------------+------------------------+
|             Destination IP (4 Bytes)            |
+-----------+------------+------------------------+
| Zero (0)  | Proto (6)  |    TCP Length (2 Bytes)|
+-----------+------------+------------------------+
|                   TCP Header                    |
+-------------------------------------------------+
|                   TCP Payload                   |
+-------------------------------------------------+
```

Pseudo Header 不會真的出現在封包中，它只是 TCP checksum 計算時臨時加入的資料，用來把 Source IP、Destination IP 與 Protocol 一起納入保護。

一旦補上 `tcp_checksum()`，Linux 核心便能接受我們的 `SYN-ACK`，順利完成三次交握。

```c
struct tcp_pseudo_hdr {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;
    uint8_t  protocol; // 6 (IPPROTO_TCP)
    uint16_t tcp_len;  // TCP Header + Payload 長度
} __attribute__((packed));

uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const void *tcp_data, int tcp_len)
{
    struct tcp_pseudo_hdr pseudo;
    pseudo.src_ip = src_ip;
    pseudo.dst_ip = dst_ip;
    pseudo.zero = 0;
    pseudo.protocol = 6;
    pseudo.tcp_len = htons((uint16_t)tcp_len);

    int total_len = sizeof(pseudo) + tcp_len;
    uint8_t buf[total_len];
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), tcp_data, tcp_len);

    return ipv4_checksum(buf, total_len);
}
```

---

# 經典應用：Echo Server 主程式

有了 Socket API 封裝後，`main()` 不再需要直接處理底層二進位封包細節，而是可以專注在應用層邏輯：

```c
int main()
{
    // 1. 初始化底層網路協定棧
    net_init();

    // 2. 建立監聽伺服器
    struct socket *listener = socket_create();
    socket_bind(listener, 8080);
    socket_listen(listener);

    printf("\n========================================\n");
    printf("   Echo Server Running on Port 8080   \n");
    printf("========================================\n\n");

    // 3. 伺服器主服務迴圈
    while (1) {
        struct socket *conn = socket_accept(listener);
        if (!conn) continue;

        uint8_t buf[1024];
        int n = socket_recv(conn, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            printf("\n[Echo Server] Received: %s\n", buf);
            socket_send(conn, buf, n); // 回射資料
        }

        socket_close(conn); // 服務結束，主動掛斷電話
    }

    return 0;
}
```

這個 Echo Server 是教學版同步模型：一次 `accept` 一條連線，處理完後主動關閉，再回到下一輪等待；尚未實作多連線並行、事件迴圈或 thread pool。

---

# 終極驗收成果

### 1. Client 端（Linux 原生 `nc` 工具）：
```bash
user@MSI:~$ nc 10.0.0.2 8080
hello!
hello!
```

若 `nc` 沒有立即返回，可按 `Ctrl+C` 結束測試；Server 端仍可觀察到主動 close 與 `TIME_WAIT` 流程。

### 2. Server 端真實運作軌跡剖析：

```text
TAP device: tap0 (UP)
TCP/IP Stack Initialized.
[Socket 0] Bound to Port 8080
[Socket 0] Listening on Port 8080

========================================
   Echo Server Running on Port 8080   
========================================

--- 階段一：ARP 解析與三次交握 ---
[ARP] Request for 10.0.0.2 received -> Generating ARP Reply
[TCP] Incoming SYN on Port 8080
[TCP] Sent SYN-ACK: SEQ=5000, ACK=836320101
[TCP] ACK Received! Handshake Complete!
[TCP] Connection Established: State -> ESTABLISHED
[Socket 1] Accepted connection from port 58526

--- 階段二：雙向資料傳輸 (Echo) ---
[TCP] Received Payload: SEQ=836320101, Len=7 (Expected SEQ=836320101)
[TCP DATA]
hello!

[Echo Server] Received: hello!
[TCP Send Buffer] Saved segment in slot [00]: SEQ=5001, Len=7
[TCP] Sent Data: 7 bytes | New SEQ=5008, ACK=836320108

--- 階段三：主動關閉與半關閉 (Half-Close) ---
[TCP Active Close] ★ Initiating Active Close: Sending FIN...
[TCP Active Close] State -> FIN_WAIT_1 (Waiting for peer ACK)
[TCP Active Close] ACK for our FIN received! (ACK=5009)
[TCP Active Close] State -> FIN_WAIT_2 (Waiting for peer FIN)

--- 階段四：四向揮手終止與 2 秒 TIME_WAIT ---
[TCP Active Close] Peer FIN received! (SEQ=836320108)
[TCP Active Close] Sending Final ACK -> State: TIME_WAIT
[TCP TIME_WAIT] ★ 2-second Timer Expired -> State: CLOSED (Socket [01] released)

=== TCP SOCKET TABLE ===
[00] State: LISTEN       | Local Port: 8080 (*:* -> :8080)
========================
```

親眼見證：
1. **握手**：`SYN` $\rightarrow$ `SYN-ACK` $\rightarrow$ `ACK`，成功建立連線。
2. **通訊**：`hello!` 成功傳遞並 Echo 回射。
3. **揮手**：Server 主動送出 `FIN` 後進入 `FIN_WAIT_1`；收到 Client 的 `ACK` 後進入 `FIN_WAIT_2`；最後收到 Peer FIN，回送最終 ACK 進入 `TIME_WAIT`，並在 2 秒後釋放槽位。

---

# 今日總結

今天我們把自製網路協定棧從「底層封包處理引擎」往前推進到「應用程式可呼叫的類 Socket API」。

至此，我們的自製網路協定棧已經具備基本的傳輸層與應用層界線：
- 應用程式不需要直接碰觸 Ethernet MAC、IP Checksum、TCP SEQ/ACK。
- 提供了類 POSIX 風格的 `socket_create()`、`socket_bind()`、`socket_listen()`、`socket_accept()`、`socket_recv()`、`socket_send()`、`socket_close()`。
- 補上 TCP Checksum 後，已能在本教學情境中與 Linux `nc` 完成基本互通。

---

# Day 26 預告

基礎建設已經具備雛形，明天我們將正式進入應用層協定：

# 應用層協定（Application Layer）— 打造自己的 HTTP Web Server

明天你將第一次用瀏覽器敲下：
```text
http://10.0.0.2:8080
```
並解析出真實的 HTTP Request：
```http
GET / HTTP/1.1
Host: 10.0.0.2:8080
User-Agent: Mozilla/5.0 ...
```
正式開啟 Web 伺服器的開發旅程！
