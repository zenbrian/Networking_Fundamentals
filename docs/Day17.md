# Day 17：TCP Socket Table 與 TCP State Machine — 賦予 TCP 連線的靈魂

昨天（Day 16）我們成功打通了 TCP 封包的接收與解析，能清楚讀懂 TCP 標頭裡的 Ports、SEQ、ACK 以及 Flags：

```text
Ethernet
    ↓
  IPv4 (Protocol 6)
    ↓
   TCP
    ↓
TCP Header (Ports, SEQ, ACK, Flags, Window)
```

然而，能看懂封包只是第一步。在真實的網路世界中，面臨著一個根本性的問題：

> **「當收到一個 TCP SYN 封包時，協定棧怎麼知道這是要找誰的新連線？」**  
> **「當收到一個 TCP ACK 或 Data 封包時，這個封包究竟屬於哪一條連線？」**

今天（Day 17），我們正式實作了作業系統核心最核心的機制 —— **TCP Socket Table（連線控制表）** 與 **TCP State Machine（狀態機）**，讓我們的 TCP 堆疊真正具備了管理連線、追蹤生命週期的能力！

---

# 今日學習目標與成果

- [x] **徹底理解 UDP 與 TCP 的根本差異**：理解為何 UDP 只要 Port，而 TCP 必須依賴 **4-Tuple（四元組：Src IP, Dst IP, Src Port, Dst Port）** 來唯一辨識一條連線。
- [x] **建立 TCP 核心狀態機列舉（`enum tcp_state`）**：
  - `TCP_CLOSED`：閒置 / 關閉
  - `TCP_LISTEN`：伺服端等待連線請求
  - `TCP_SYN_SENT`：客戶端發出 SYN，等待 SYN-ACK
  - `TCP_SYN_RECEIVED`：伺服端收到 SYN，等待最後握手 ACK
  - `TCP_ESTABLISHED`：交握完成，可傳輸資料
- [x] **設計 TCP 連線控制結構體（`struct tcp_socket`）**：記錄 4-Tuple 通訊端點與當前狀態。
- [x] **實作 TCP Socket Table 與容量管理（`tcp_init`）**：以固定陣列管理 64 個 Socket 槽位。
- [x] **實作伺服端監聽機制（`tcp_listen`）**：配置第一個空閒 Socket 並掛牌為 `TCP_LISTEN` 服務台。
- [x] **實作監聽端點查詢（`tcp_find_listener`）**：收到新封包時，走訪 Table 尋找對應的接待埠，以指標（Pointer）回傳控制權。
- [x] **實作新連線動態建立（`tcp_create_connection`）**：收到 SYN 時絕不破壞原有的 LISTEN 服務台，而是獨立開闢新房間，轉移為 `TCP_SYN_RECEIVED` 狀態。
- [x] **除錯觀察工具與踩坑修復（`tcp_dump_table`）**：
  - 印出所有活躍的 Socket 與 4-Tuple 連線路徑。
  - 識破並修復 C 語言老函式 `inet_ntoa()` static buffer 共享覆蓋的大坑，全面升級為安全的 `inet_ntop()`。
- [x] **全鏈路連線追蹤驗收成功**：
  - `LISTEN` Socket 維持在 `*:* -> :8080`。
  - 收到第 1 包 SYN：成功建立 `10.0.0.1:52144 -> 10.0.0.2:8080 (SYN_RECEIVED)`。
  - 收到第 2 包 SYN：成功分配新槽位建立獨立連線，證明具備多連線管理能力！

---

# 核心概念深入剖析

### 1. 為什麼 UDP 只要 Port，TCP 卻非要 4-Tuple 不可？

* **UDP 是無連線的（Connectionless）**：  
  只要目的 Port 是 `8080`，不管是誰寄來的，直接把封包扔進 `udp_socket[8080]` 的緩衝區即可。
* **TCP 是連線導向的（Connection-Oriented）**：  
  同一台 Web Server 的 `Port 80`，可能同時與數千個不同的 Client 保持通訊：
  - Client A (`192.168.1.10:52341`) $\rightarrow$ Server (`192.168.1.100:80`)
  - Client B (`192.168.1.11:52342`) $\rightarrow$ Server (`192.168.1.100:80`)
  
  兩條連線的目的 Port 全都是 `80`，若只靠 Port 根本無法分清這包資料該交給 Client A 還是 Client B。因此，TCP 必須靠 **4-Tuple（四元組）** 作為唯一身份證：
  $$\text{4-Tuple} = (\text{Src IP}, \text{Dst IP}, \text{Src Port}, \text{Dst Port})$$

---

### 2. 生動的大樓管理員比喻：TCP 狀態機如何運作？

你可以把我們的 `tcp_table[64]` 想像成一棟大樓裡的 **64 間辦公室**：

```text
[ 大樓初始化 (tcp_init) ]
  ├── 64 間辦公室大門深鎖
  └── 全部掛牌：TCP_CLOSED (無人使用)

[ 伺服端開門營業 (tcp_listen(8080)) ]
  ├── 管理員挑了第 0 號空房間
  └── 門口掛牌：TCP_LISTEN (8080 號專屬接待處，不辦一般業務，專門等新客人敲門)

[ 客人敲門 (收到 SYN 封包，找 8080) ]
  ├── 1. 管理員巡邏確認：大樓有沒有 8080 接待處？ (tcp_find_listener)
  │      └── 找到了！在第 0 號房！
  ├── 2. 關鍵原則：不能佔用接待處！
  │      └── 接待處必須繼續留在原地，等待下一個客人連線。
  └── 3. 開闢獨立會談室 (tcp_create_connection)
         ├── 管理員往後找第 1 號空房
         ├── 把客人的身份證 (4-Tuple: 10.0.0.1:52144 -> 10.0.0.2:8080) 貼在門牌上
         └── 狀態掛牌：TCP_SYN_RECEIVED (手續辦理中，等待最後握手完成)

[ 以後客人寄信 (收到 ACK / Data 封包) ]
  └── 管理員看信封上的 4-Tuple，挨個核對門牌，直接把信塞進第 1 號房！
```

---

# 資料結構與實作拆解

### 1. 狀態列舉與 Socket 結構 (`include/tcp.h`)

```c
/* TCP 狀態機列舉 */
enum tcp_state
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED
};

/* TCP 連線控制區塊 (TCB / Socket) */
struct tcp_socket
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    enum tcp_state state;
};
```

---

### 2. 狀態表初始化與監聽管理 (`src/tcp.c`)

```c
#define MAX_TCP_SOCKETS 64

static struct tcp_socket tcp_table[MAX_TCP_SOCKETS];

/* 初始化：所有房間歸零為 CLOSED */
void tcp_init(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        tcp_table[i].state = TCP_CLOSED;
    }
}

/* 註冊監聽服務台 */
int tcp_listen(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_LISTEN;
            tcp_table[i].dst_port = port;
            return 0; // 只配置一間接待處即刻返回
        }
    }
    return -1;
}

/* 尋找接待處：回傳該房間的指標 (Pointer) */
struct tcp_socket* tcp_find_listener(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_LISTEN && tcp_table[i].dst_port == port) {
            return &tcp_table[i];
        }
    }
    return NULL;
}
```

---

### 3. 動態建立連線與狀態移轉 (`src/tcp.c`)

```c
struct tcp_socket* tcp_create_connection(uint32_t src_ip, uint32_t dst_ip,
                                          uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_SYN_RECEIVED;
            tcp_table[i].src_ip = src_ip;
            tcp_table[i].dst_ip = dst_ip;
            tcp_table[i].src_port = src_port;
            tcp_table[i].dst_port = dst_port;
            return &tcp_table[i];
        }
    }
    return NULL;
}
```

---

### 4. 封包接收分流與狀態機串接 (`tcp_receive`)

```c
void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    (void)fd;

    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ip_hdr_len + sizeof(struct tcp_hdr)) {
        printf("[TCP] Packet too short\n");
        return;
    }

    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + ip_hdr_len);
    tcp_print_header(tcp);

    /* 處理 SYN 連線請求 */
    if (tcp->flags & TCP_SYN) {
        uint16_t dst_port = ntohs(tcp->dst_port);
        uint16_t src_port = ntohs(tcp->src_port);

        // 1. 檢查是否有該 Port 的服務台正在監聽
        struct tcp_socket *listener = tcp_find_listener(dst_port);
        if (listener == NULL) {
            printf("[TCP] Port %u not listening -> DROP\n", dst_port);
            return;
        }

        printf("[TCP] Incoming SYN on Port %u\n", dst_port);

        // 2. 建立全新 Connection，狀態變為 SYN_RECEIVED
        struct tcp_socket *conn = tcp_create_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] Connection table full!\n");
            return;
        }

        // 3. 印出目前 Socket Table 狀態
        tcp_dump_table();
        return;
    }
}
```

---

# 踩坑經驗與底層調校筆記

### 🚨 C 語言經典陷阱：`inet_ntoa()` static buffer 共享覆蓋

#### 【現象】
在最初實作 `tcp_dump_table()` 時，畫面上印出：
```text
[01] State: SYN_RECEIVED | 10.0.0.1:52144 -> 10.0.0.1:8080
```
明明目的 IP 是本機 `10.0.0.2`，為什麼兩邊都印成 `10.0.0.1`？

#### 【原因】
`inet_ntoa()` 是幾十年前設計的老函式，內部使用了一塊**靜態記憶體緩衝區（static buffer）**。
當在同一個 `printf` 呼叫裡連續兩次呼叫 `inet_ntoa(sip)` 和 `inet_ntoa(dip)` 時：
兩個 `%s` 其實都指向了同一個記憶體位址！後者求值時直接覆蓋了前者的內容，導致輸出結果完全錯誤。

#### 【修復方式】
改用 POSIX 現代執行緒安全函式 `inet_ntop()`，分配兩塊各自獨立的 buffer，徹底根絕覆蓋問題：

```c
char sip_str[INET_ADDRSTRLEN];
char dip_str[INET_ADDRSTRLEN];

inet_ntop(AF_INET, &tcp_table[i].src_ip, sip_str, sizeof(sip_str));
inet_ntop(AF_INET, &tcp_table[i].dst_ip, dip_str, sizeof(dip_str));

printf("| %s:%u -> %s:%u\n",
       sip_str, tcp_table[i].src_port,
       dip_str, tcp_table[i].dst_port);
```

---

# 驗收實測結果

### 終端機 1：Protocol Stack (`./network`) 輸出

```text
=== Routing Table ===
Destination        Netmask            Gateway
----------------------------------------------------------
10.0.0.0           255.255.255.0      0.0.0.0 (Direct)
0.0.0.0            0.0.0.0            10.0.0.1
=====================

[UDP] Bound to port 8080
TAP device: tap0 (UP)
Ethernet header size: 14 bytes
Waiting for Ethernet frame...

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

Frame length: 54 bytes
```

### 結果分析
1. **LISTEN 不滅**：`[00]` 號槽位維持 `LISTEN`，表示服務台能持續接待後續連線。
2. **狀態正確移轉**：`[01]` 號槽位成功誕生，狀態被標記為 `SYN_RECEIVED`，4-Tuple（`10.0.0.1:52144 -> 10.0.0.2:8080`）紀錄完全正確。
3. **多連線支援**：再次發送 SYN 時，自動配置 `[02]` 號槽位，連線隔離機制驗證成功！

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
                        ICMP (1)      UDP (17)      TCP (6)
                            │            │            │
                       Echo Reply     DNS / App   Header Parser
                                                      ↓
                                              Socket Table Lookup  <-- 【Day 17】
                                                      ↓
                                             TCP State Machine
                                          ┌───────────┴───────────┐
                                          │                       │
                                        LISTEN              SYN_RECEIVED
                                      (服務接待處)           (新連線建立中)
```

---

# Day 18 預告：TCP Three-Way Handshake (三方交握) — 誕生第一包 SYN-ACK！

今天我們完成了「收到 SYN 並記錄狀態」。  
明天，我們將讓協定棧第一次開口說話：

1. **實作 TCP Checksum（含 IPv4 虛擬標頭 Pseudo Header）**：這是傳輸層最複雜的校驗機制。
2. **主動組裝 TCP SYN-ACK 封包**：
   - 設定 `Flags = SYN | ACK`
   - 計算伺服端本機的初始序號（Initial Sequence Number, ISN）
   - 回覆對方的確認序號：$\text{ACK Number} = \text{Client SEQ} + 1$
3. **射出 SYN-ACK 回覆 Client**，跨出建立 TCP 三方交握的最關鍵一步！
