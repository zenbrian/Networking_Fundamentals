# 用 Google AI 從零打造 Network Stack

> 2026 iThome 鐵人賽 Build on Google AI 組別主題：  
> **Gemini 當教授，Antigravity CLI 當助教，30 天從 TAP Device 一路實作到 HTTP Server。**

這個專案是一個 30 天網路底層實作挑戰。目標不是只閱讀網路理論，而是透過 C 語言與 Linux TAP Device，親手打造一個可觀察、可驗證、可逐步擴充的迷你 Network Stack。

最終目標是在 Day 30 完成以下路徑：

```text
Web Browser
    ↓
HTTP
    ↓
TCP
    ↓
IPv4
    ↓
Ethernet
    ↓
TAP Device
    ↓
My Network Stack
```

透過這條路徑，逐步理解資料如何從應用層被封裝、傳遞、解析，最後由自己實作的 Network Stack 處理。

---

## Google AI 在這個專案中的角色

本系列報名 **Build on Google AI** 組別，因此 AI 並不是單純用來產生文章，而是被設計成實際參與學習與實作流程的協作角色。

| 角色 | 工具 | 工作內容 |
|---|---|---|
| AI 教授 | Google Gemini | 規劃每日課程、拆解概念、安排實作目標、銜接下一階段學習 |
| 程式碼助教 | Antigravity CLI | 審查 C 語言程式碼、檢查封包處理邏輯、協助 debug、避免污染 Gemini 主上下文 |
| 學生 / 實作者 | 我 | 理解概念、撰寫程式、執行實驗、使用 Wireshark/tcpdump 驗證結果 |

這樣的分工讓 Gemini 可以維持在「課程教授」的高度，負責學習路線與概念脈絡；而 Antigravity CLI 則負責深入程式碼細節與除錯，避免大量 code review 討論干擾主線教學上下文。

---

## 課程特色

- 從 TAP Device 開始，理解封包如何進出 Linux Kernel。
- 使用 C 語言逐步解析與產生 Ethernet、ARP、IPv4、ICMP、UDP、TCP 封包。
- 每一天都盡量保留可以用 Wireshark 或 tcpdump 驗證的成果。
- 不寫 Driver、不碰 DMA、不深入作業系統記憶體管理，專注在網路工程師需要理解的封包路徑。
- 最終完成一個可以被瀏覽器連線的自製 Network Stack。

---

## 30 天課程規劃

### Phase 1：封包世界入門（Day 1 ~ Day 5）

目標：理解網路上的每個封包其實都是 Bytes，並透過 TAP Device 觀察 Ethernet Frame。

| Day | 主題 | 目標 |
|---|---|---|
| [Day 01](docs/Day01.md) | TAP Device | 建立 tap0，使用 `read()` 接收 Linux Kernel 送出的原始封包 |
| [Day 02](docs/Day02.md) | Ethernet Header | 解析 Destination MAC、Source MAC、EtherType |
| [Day 03](docs/Day03.md) | Ethernet Frame | 實作 Ethernet Frame 接收判斷與測試情境 |
| [Day 04](docs/Day04.md) | ARP Request | 理解 IP 到 MAC 的查詢流程 |
| [Day 05](docs/Day05.md) | ARP Reply | 建立 ARP Table 並儲存 IP/MAC 對應 |

### Phase 2：IPv4（Day 6 ~ Day 11）

目標：從 Ethernet Payload 進入 IPv4，理解 IP Header、Checksum、ICMP 與 Routing。

| Day | 主題 | 目標 |
|---|---|---|
| [Day 06](docs/Day06.md) | IPv4 Header | 解析 Version、TTL、Protocol、Src IP、Dst IP |
| [Day 07](docs/Day07.md) | IPv4 Checksum | 實作 RFC 791 Header Checksum |
| [Day 08](docs/Day08.md) | ICMP Header | 解析 Echo Request 與 Echo Reply |
| [Day 09](docs/Day09.md) | Ping Reply | 回應 `ping 10.0.0.2` |
| Day 10 | TTL | 實作 TTL 遞減並理解 Traceroute 原理 |
| Day 11 | Routing | 理解 Local Network、Gateway、Default Route |

### Phase 3：UDP（Day 12 ~ Day 15）

目標：實作無連線傳輸，並用 Mini DNS Client 驗證 UDP 的實際用途。

| Day | 主題 | 目標 |
|---|---|---|
| [Day 12](docs/Day12.md) | UDP Header | 解析 Source Port、Destination Port、Length、Checksum |
| [Day 13](docs/Day13.md) | UDP Receiver | 接收 UDP 封包並交付 Application Callback |
| [Day 14](docs/Day14.md) | UDP Sender | 主動送出 UDP 封包，並用 `nc -lu 9999` 驗證 |
| Day 15 | Mini DNS Client | 查詢 `google.com` 並解析 DNS 回應 |

### Phase 4：TCP 核心（Day 16 ~ Day 25）

目標：實作 TCP Header、狀態機、三向交握、資料接收、序號管理、重傳與連線關閉。

| Day | 主題 | 目標 |
|---|---|---|
| Day 16 | TCP Header | 解析 SEQ、ACK、Window、Flags |
| Day 17 | TCP State Machine | 建立 LISTEN、SYN_RECEIVED、ESTABLISHED 狀態 |
| Day 18 | SYN | 接收 SYN 封包 |
| Day 19 | SYN-ACK | 回覆 SYN ACK |
| Day 20 | 3-Way Handshake | 完成 TCP 三向交握 |
| Day 21 | TCP Payload | 接收 TCP Data |
| Day 22 | Sequence Number | 管理 SEQ 與 ACK |
| Day 23 | TCP Retransmission | 實作 Timeout 與 Retransmit |
| Day 24 | FIN | 實作 Connection Close |
| Day 25 | Socket API | 封裝 `bind()`、`listen()`、`accept()`、`recv()`、`send()` |

### Phase 5：HTTP（Day 26 ~ Day 30）

目標：在自製 TCP/IP Stack 上整合 HTTP 應用層處理，串起從封包到應用回應的完整流程。

| Day | 主題 | 目標 |
|---|---|---|
| Day 26 | HTTP Request | 解析 `GET /` |
| Day 27 | HTTP Response | 回覆 `HTTP/1.1 200 OK` |
| Day 28 | Dynamic Response | 產生 JSON 回應 |
| Day 29 | Static File Server | 回傳 `index.html` |
| Day 30 | Final Project | 完成自製 Network Stack 與 HTTP 應用整合 |

---

## 目前進度

目前已完成並整理到文件中的內容：

- [x] Day 01：TAP Device
- [x] Day 02：Ethernet Header
- [x] Day 03：Ethernet Frame
- [x] Day 04：ARP Request
- [x] Day 05：ARP Reply
- [x] Day 06：IPv4 Header
- [x] Day 07：IPv4 Checksum
- [x] Day 08：ICMP Header
- [x] Day 09：Ping Reply

---

## 專案結構

```text
.
├── docs/          # 每日教學文章
├── docs/images/   # 文章圖片
├── include/       # Header files
├── src/           # Network Stack 實作
├── test/          # 測試與封包產生工具
└── Makefile
```

---

## 適合讀者

這個系列適合想理解以下主題的人：

- 網路封包分析
- Wireshark / tcpdump 實戰
- Linux TAP Device
- Ethernet / ARP / IPv4 / ICMP / UDP / TCP
- C 語言網路底層實作
- 後端、SRE、DevOps、雲端網路工程基礎

---

## 學完後希望真正理解

```text
Application Layer
    HTTP
        ↓
Transport Layer
    TCP
    UDP
        ↓
Network Layer
    IPv4
    ICMP
        ↓
Data Link Layer
    Ethernet
    ARP
        ↓
Physical / Virtual Link
    TAP Device
```

這是一趟從 Bytes、封包、協定，到 HTTP Server 的完整網路底層實作旅程。
