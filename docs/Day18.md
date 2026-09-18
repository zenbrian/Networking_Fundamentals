# Day 18：TCP Three-Way Handshake (Part 1) — 收到 SYN，回覆 SYN-ACK

昨天（Day 17）我們完成了 TCP Socket Table 與狀態機，能夠在收到 SYN 時動態建立 `TCP_SYN_RECEIVED` 的連線槽位。但當時伺服器就像是個「已讀不回」的冷漠客服 —— 雖然在內部做好了登記，卻**一句話都沒回覆對方**。

今天（Day 18），我們正式讓自製的網路協定棧開口說話，邁出 TCP 三向交握中最關鍵的第二步：
**由伺服端組裝並發送標準的 `SYN + ACK` 封包！**

```text
Client                                  Server
  │                                       │
  │ 1. SYN (SEQ=1000)                     │
  ├──────────────────────────────────────>│  (找到 LISTEN Socket -> 建立新連線 -> SYN_RECEIVED)
  │                                       │
  │ 2. SYN-ACK (SEQ=5000, ACK=1001)       │
  │<──────────────────────────────────────┤  <-- 【今日核心成果！】
  │                                       │
  │ 3. ACK                                │
  │    (Day 19 即將完成)                   │
```

---

# 今日學習目標與成果

- [x] **徹底理解 SYN、ACK 與 SYN-ACK 的語義**：
  - **SYN (Synchronize)**：同步序號，雙方通訊前進行「對錶」，約定起始 Sequence Number。
  - **ACK (Acknowledgment)**：確認收到對方的訊號或資料。
  - **SYN-ACK**：一兼二顧！既確認對方的連線請求，同時也告知對方我方的手錶序號（Initial Sequence Number, ISN）。
- [x] **釐清序號消耗機制（為什麼 ACK 是 Client SEQ + 1）**：
  - 即使 SYN 封包 Payload 長度為 0，但依照 RFC 793 規定，**SYN 與 FIN 控制旗標各自必須消耗 1 個 Sequence Number**（將其視為一張建立契約）。
- [x] **破除常見迷思：三向交握是否有帶 Data？**：
  - 交握過程原則上**不帶任何應用層資料（Length = 0）**。
  - 核心目的純粹是**確認雙方的雙向收發能力均暢通**，交握完成進入 `ESTABLISHED` 後才開始傳送真實 Data。
- [x] **實作 `tcp_send_syn_ack()` 封包發射器**：
  - 由內而外完成 L4 TCP Header $\rightarrow$ L3 IPv4 Header $\rightarrow$ L2 Ethernet Header 封裝。
  - 正確將來源與目的 IP / Port 反轉（Server $\rightarrow$ Client）。
  - 設定 Flags 為 `TCP_SYN | TCP_ACK`（`0x12`）。
  - 本日先聚焦交握封包格式與狀態流程，TCP Checksum 仍暫填 `0`，完整 Pseudo Header Checksum 留待後續補強。
- [x] **無縫整合 ARP 動態學習機制**：
  - 收到 SYN 時第一時間將 Client 的 IP 與 MAC 記錄到 `arp_table`（`arp_table_insert`），回覆時直接命中快取。
- [x] **雙終端機與 tcpdump 全鏈路實測驗收**：
  - 終端機順利印出 `[TCP] Sent SYN-ACK: SEQ=5000, ACK=1001`。
  - `tcpdump` 抓包親眼見證：`Flags [S.]`（SYN+ACK）飛越虛擬網卡！

---

# 核心概念深入剖析

### 1. 三向交握（Three-Way Handshake）在確認什麼？

為什麼 TCP 不能只交握兩次？一定要三次？  
想像兩個人在山頭用無線電對話，在講正事（傳資料）前必須確認通道暢通：

```text
1. Client 呼叫 (SYN, SEQ=1000)：
   「喂喂喂，Server，你聽得到我說話嗎？我講話的序號從 1000 號開始算。」
   👉 Server 知道：Client 能發話、Server 能收聽。

2. Server 回覆 (SYN-ACK, SEQ=5000, ACK=1001)：
   「聽得很清楚！你下次從 1001 開始講。那我發話你聽得到嗎？我講話的序號從 5000 號開始算。」
   👉 Client 收到後知道：Server 能發話、Client 能收聽！而且剛才自己的話 Server 有收到！

3. Client 最後確認 (ACK, ACK=5001)：
   「我也聽得很清楚！你下次從 5001 開始講。」
   👉 Server 收到後知道：原來 Client 也能順利聽到我剛剛說的話！
```

經過這三步：
* **雙方發送能力與接收能力全數確認正常**。
* **雙方的 Sequence Numbers 雙向對齊同步**。
* 三步完成後，雙方才正式開始傳遞 Payload Data！

---

### 2. 封包標頭欄位反轉設計

從 Server 回覆 SYN-ACK 時，所有「來源」與「目的」必須嚴格反轉：

| 欄位 | Client 送進來的 SYN | Server 回覆的 SYN-ACK | 說明 |
| :--- | :--- | :--- | :--- |
| **Ethernet Dst MAC** | Server MAC (`02:00:00:00:00:01`) | Client MAC (`52:54:00:12:34:56`) | 從 ARP 表中查得 |
| **Ethernet Src MAC** | Client MAC (`52:54:00:12:34:56`) | Server MAC (`LOCAL_MAC`) | 本機網卡 MAC |
| **IPv4 Dst IP** | `10.0.0.2` | `10.0.0.1` | 送回給 Client |
| **IPv4 Src IP** | `10.0.0.1` | `10.0.0.2` | 本機 IP |
| **TCP Dst Port** | `8080` | `52144` | 送回 Client 的臨時埠 |
| **TCP Src Port** | `52144` | `8080` | 本機監聽埠 |
| **TCP Flags** | `0x02` (`SYN`) | `0x12` (`SYN | ACK`) | 雙旗標同時打勾 |
| **TCP SEQ** | `1000` | `5000` | Server 自己的起始序號 |
| **TCP ACK** | `0` | `1001` | $\text{Client SEQ} + 1$ |

> **Checksum 補充**：IPv4 Header Checksum 與 TCP Checksum 是兩件事。本文程式碼已重算 IPv4 Header Checksum，但 TCP Header 內的 `checksum` 仍暫填 `0`。這是 Day18～Day21 測試環境的教學簡化；真實 TCP 封包必須計算包含 Pseudo Header、TCP Header 與 Payload 的 TCP Checksum，否則一般 OS TCP Stack 可能會丟棄封包。

---

# 核心程式碼實作

### 1. 擴充 Socket 資料結構 (`include/tcp.h`)

在 `struct tcp_socket` 中新增 `seq` 與 `ack`，用以追蹤雙方的序列號進度：

```c
struct tcp_socket
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;  // Server 自己的序列號
    uint32_t ack;  // 期待收到的對方序列號
    enum tcp_state state;
};

int tcp_send_syn_ack(int fd, struct tcp_socket *conn);
```

---

### 2. 實作 SYN-ACK 封包發送函式 (`src/tcp.c`)

```c
int tcp_send_syn_ack(int fd, struct tcp_socket *conn)
{
    // 1. 準備 Buffer 與切割各層指標 (14 + 20 + 20 = 54 Bytes)
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 2. 封裝 Layer 4: TCP Header
    tcp->src_port = htons(conn->dst_port); // 8080
    tcp->dst_port = htons(conn->src_port); // 52144
    tcp->seq = htonl(conn->seq);           // 5000
    tcp->ack = htonl(conn->ack);           // 1001
    tcp->data_offset = (5 << 4);           // 20 Bytes (5 words)
    tcp->window = htons(4096);             // 初始視窗大小
    tcp->flags = (TCP_ACK | TCP_SYN);      // 0x12: SYN + ACK
    tcp->checksum = 0;                     // 暫設為 0
    tcp->urgent_ptr = 0;

    // 3. 封裝 Layer 3: IPv4 Header
    ip->version_ihl = (4 << 4) | 5;        // Version 4, IHL 5
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)); // 40 Bytes
    ip->identification = htons(2001);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;            // Protocol 6
    ip->src_ip = conn->dst_ip;             // 來源：10.0.0.2
    ip->dst_ip = conn->src_ip;             // 目的：10.0.0.1
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 4. 封裝 Layer 2: Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send SYN-ACK failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN); // Client MAC
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);  // 本機 MAC
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 5. 寫出到 TAP 虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write SYN-ACK failed");
        return -1;
    }

    printf("[TCP] Sent SYN-ACK: SEQ=%u, ACK=%u\n", conn->seq, conn->ack);
    return 0;
}
```

---

### 3. 接收分流器整合 ARP 學習與觸發發送 (`tcp_receive`)

```c
void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    if (len < ETH_HEADER_LEN + ip_hdr_len + sizeof(struct tcp_hdr)) {
        printf("[TCP] Packet too short\n");
        return;
    }

    // ⭐ 動態 ARP 學習：收到封包時立刻記下 Client 的 IP 與 MAC
    struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)buffer;
    arp_table_insert((const uint8_t *)&ip->src_ip, rx_eth->src);

    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + ip_hdr_len);
    tcp_print_header(tcp);

    if (tcp->flags & TCP_SYN) {
        uint16_t dst_port = ntohs(tcp->dst_port);
        uint16_t src_port = ntohs(tcp->src_port);

        struct tcp_socket *listener = tcp_find_listener(dst_port);
        if (listener == NULL) {
            printf("[TCP] Port %u not listening -> DROP\n", dst_port);
            return;
        }

        printf("[TCP] Incoming SYN on Port %u\n", dst_port);
        struct tcp_socket *conn = tcp_create_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] Connection table full!\n");
            return;
        }

        // ⭐ 計算雙方序號
        uint32_t client_seq = ntohl(tcp->seq);
        conn->ack = client_seq + 1; // 消耗 1 個序號
        conn->seq = 5000;           // 本機初始序號

        tcp_dump_table();

        // ⭐ 觸發回覆 SYN-ACK！
        tcp_send_syn_ack(fd, conn);

        return;
    }
}
```

---

# 驗收實測結果

### 終端機 1：協定棧輸出 (`./network`)

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
```

---

### 終端機 2：`tcpdump` 真實網路封包抓包

```text
user@MSI:/mnt/c/Users/zenboen/Tutorial/Networking_Fundamentals$ sudo tcpdump -i tap0 -nn tcp
listening on tap0, link-type EN10MB (Ethernet), snapshot length 262144 bytes

16:50:48.558938 IP 10.0.0.1.52144 > 10.0.0.2.8080: Flags [S], seq 1000, win 4096, length 0
16:50:48.559301 IP 10.0.0.2.8080 > 10.0.0.1.52144: Flags [S.], seq 5000, ack 1001, win 4096, length 0
```

### 結果分析
1. **第 1 包（Client $\rightarrow$ Server）**：
   - `Flags [S]` 代表 **SYN** 旗標。
   - `seq 1000`，長度為 0。
2. **第 2 包（Server $\rightarrow$ Client）—— 我們的協定棧產物！**：
   - `Flags [S.]` 代表 **SYN + ACK** 雙旗標！
   - `seq 5000` 精準對應我們設定的 ISN。
   - `ack 1001` 精準確認了 Client 的第 1000 號連線契約！

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
             ▲              │            │            │
             │          ICMP (1)      UDP (17)      TCP (6)
      (動態學習對方的 MAC)  │            │            │
                       Echo Reply     DNS / App   Header Parser
                                                      ↓
                                              Socket Table Lookup
                                                      ↓
                                             TCP State Machine
                                          ┌───────────┴───────────┐
                                          │                       │
                                        LISTEN              SYN_RECEIVED
                                      (服務接待處)                 │
                                                                  ▼
                                                          tcp_send_syn_ack()  <-- 【Day 18】
                                                          (回覆 Flags: [S.])
```

---

# Day 19 預告：完成三向交握 — 迎來 ESTABLISHED 狀態！

今天我們完成了「SYN $\rightarrow$ SYN-ACK」這前兩步。  
明天，我們將收下三向交握的最後一塊拼圖：

```text
Client                     Server

SYN ----------------------> (State: SYN_RECEIVED)

     <---------------- SYN-ACK

ACK ----------------------> (State: ESTABLISHED 🎉)
```

1. **處理交握最後的 ACK 封包**：
   - 尋找處於 `SYN_RECEIVED` 狀態的連線 Socket。
   - 驗證 Client 的 ACK 號碼是否正確確認了我們的 `seq + 1`（5001）。
2. **連線狀態正式轉移為 `TCP_ESTABLISHED`**！
3. 你的協定棧將第一次建立起一條**具備最小化三向交握狀態轉換能力的 TCP 連線**，為後續傳輸 HTTP 資料鋪平道路！
