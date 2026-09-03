# Day 06：IPv4 Header 解析 —— 正式進入 Network Layer（Layer 3）

昨天我們完成了 Layer 2 的 **ARP Reply 解析** 與 **ARP Table 快取機制**。

從今天開始，我們將正式跨越資料鏈結層（Data Link Layer），進入 **OSI Layer 3：Network Layer**。

第一個要面對的協定，就是整個 Internet 的核心：

```text
IPv4
(Internet Protocol Version 4)
```

在前幾天的課程中，我們已經能夠：

```text
Ethernet Frame
      ↓
EtherType
      ↓
ARP
```

而今天開始，我們要讓 Ethernet Dispatcher 能夠辨識：

```text
EtherType = 0x0800
```

並將封包交給 IPv4 模組進行解析。

---

# 今日學習目標

完成以下內容：

* [x] 建立 `include/ipv4.h`
* [x] 定義 `struct ipv4_hdr`
* [x] 驗證 `sizeof(struct ipv4_hdr) == 20`
* [x] 解析 IPv4 Version
* [x] 解析 IPv4 Header Length（IHL）
* [x] 解析 TTL
* [x] 解析 Protocol
* [x] 解析 Source IP
* [x] 解析 Destination IP
* [x] 建立 Ethernet → IPv4 Dispatcher
* [x] 成功顯示 IPv4 Header 資訊

---

# 1. IPv4 在網路堆疊中的位置

目前我們的網路堆疊已經成長為：

```text
Application
      ↑
IPv4
      ↑
Ethernet
      ↑
TAP Device
```

當 Linux 核心收到 Ethernet Frame 時：

```text
Ethernet Frame
      ↓
EtherType
      ↓
0x0800 ?
      ↓
IPv4 Layer
```

只要 EtherType 為：

```text
0x0800
```

就代表 Ethernet Payload 裡面裝的是：

```text
IPv4 Packet
```

---

# 2. IPv4 Packet 結構

IPv4 Header 的最小長度為：

```text
20 Bytes
```

結構如下：

```text
┌─────────────────────────────┐
│ Version + IHL (1)           │
├─────────────────────────────┤
│ Type of Service (1)         │
├─────────────────────────────┤
│ Total Length (2)            │
├─────────────────────────────┤
│ Identification (2)          │
├─────────────────────────────┤
│ Flags + Fragment (2)        │
├─────────────────────────────┤
│ TTL (1)                     │
├─────────────────────────────┤
│ Protocol (1)                │
├─────────────────────────────┤
│ Header Checksum (2)         │
├─────────────────────────────┤
│ Source IP (4)               │
├─────────────────────────────┤
│ Destination IP (4)          │
└─────────────────────────────┘
```

---

## 2.1 今天重點解析哪些欄位？

今天先聚焦在最重要的五個欄位：

| 欄位                      | 說明        |
| ----------------------- | --------- |
| Version                 | IP 協定版本   |
| IHL                     | Header 長度 |
| TTL                     | 存活跳數      |
| Protocol                | 上層協定      |
| Source / Destination IP | 來源與目的位址   |

---

## Version

IPv4 固定為：

```text
4
```

IPv6 則為：

```text
6
```

在 Header 中：

```c
uint8_t version_ihl;
```

高 4 Bits 存放 Version。

因此：

```c
version = ip->version_ihl >> 4;
```

---

## IHL（Internet Header Length）

低 4 Bits 代表 Header 長度。

取得方式：

```c
ihl = ip->version_ihl & 0x0F;
```

單位不是 Byte，而是：

```text
4 Bytes
```

因此：

```c
ihl * 4
```

才是真正 Header 長度。

---

例如：

```text
0x45
```

拆開後：

```text
0100 0101

Version = 4
IHL     = 5
```

因此：

```text
Header Length

5 × 4

=

20 Bytes
```

---

## TTL（Time To Live）

TTL 用來避免封包在網路中無限循環。

每經過一台 Router：

```text
TTL - 1
```

當 TTL 變成：

```text
0
```

封包會被丟棄。

常見數值：

| 作業系統    | TTL |
| ------- | --- |
| Linux   | 64  |
| Windows | 128 |
| Cisco   | 255 |

---

## Protocol

IPv4 Header 中的：

```c
uint8_t protocol;
```

代表上層協定類型。

常見值：

| 數值 | 協定   |
| -- | ---- |
| 1  | ICMP |
| 6  | TCP  |
| 17 | UDP  |

因此：

```text
Ethernet
    ↓
IPv4
    ↓
Protocol = 1
    ↓
ICMP
```

---

# 3. 記憶體中的封包結構

今天的測試程式會建立一個：

```text
34 Bytes
```

的 Ethernet Frame。

組成如下：

```text
14 Bytes Ethernet Header
+
20 Bytes IPv4 Header
=
34 Bytes
```

---

## 記憶體配置

```text
┌────────────────────────────────────┐
│ Ethernet Header (14 Bytes)         │
├────────────────────────────────────┤
│ Destination MAC                    │
│ Source MAC                         │
│ EtherType = 0x0800                 │
├────────────────────────────────────┤
│ IPv4 Header (20 Bytes)             │
├────────────────────────────────────┤
│ Version / IHL                      │
│ TTL                                │
│ Protocol                           │
│ Source IP                          │
│ Destination IP                     │
└────────────────────────────────────┘
```

因此：

```c
struct ethernet_hdr *eth =
    (struct ethernet_hdr *)frame;

struct ipv4_hdr *ip =
    (struct ipv4_hdr *)(frame + ETH_HEADER_LEN);
```

即可直接定位到 IPv4 Header。

---

# 4. 建立 IPv4 Header

檔案：

```text
include/ipv4.h
```

---

## 完整結構定義

```c
struct ipv4_hdr {

    uint8_t  version_ihl;
    uint8_t  tos;

    uint16_t total_length;
    uint16_t identification;
    uint16_t flags_fragment;

    uint8_t  ttl;
    uint8_t  protocol;

    uint16_t checksum;

    uint32_t src_ip;
    uint32_t dst_ip;

} __attribute__((packed));
```

---

## 驗證大小

```c
printf("%lu\n",
       sizeof(struct ipv4_hdr));
```

輸出：

```text
20
```

表示 Header 定義正確。

---

# 5. 實作 IPv4 Header Parser

檔案：

```text
src/ipv4.c
```

---

## 解析 Version

```c
uint8_t version =
    ip->version_ihl >> 4;
```

---

## 解析 IHL

```c
uint8_t ihl =
    ip->version_ihl & 0x0F;
```

---

## 解析 Source IP

因為 IPv4 位址以 Network Byte Order 儲存：

```c
ip = ntohl(ip);
```

之後再拆成：

```c
A.B.C.D
```

格式輸出。

---

## 顯示結果

```text
IPv4 Packet
------------------
Version       : 4
Header Length : 20 bytes
TTL           : 64
Protocol      : 1
Source IP     : 10.0.0.1
Destination IP: 10.0.0.2
```

---

# 6. 更新 Ethernet Dispatcher

檔案：

```text
src/tap.c
```

新增：

```c
case ETHERTYPE_IPV4:
```

分流邏輯。

---

## Dispatcher 架構

```text
Ethernet Frame
       │
       ▼
 EtherType
       │
 ┌─────┴─────┐
 │           │
 ▼           ▼
ARP         IPv4
```

程式：

```c
switch (ntohs(eth->ethertype)) {

    case ETHERTYPE_ARP:
        arp_receive(
            payload,
            payload_len
        );
        break;

    case ETHERTYPE_IPV4:

        if (payload_len >=
            sizeof(struct ipv4_hdr)) {

            const struct ipv4_hdr *ip =
                (const struct ipv4_hdr *)payload;

            ipv4_print_header(ip);
        }

        break;
}
```

---

# 7. 建立 IPv4 測試封包

檔案：

```text
tests/send_ipv4.c
```

今天我們第一次手動建立：

```text
IPv4 Header
```

並送往：

```text
tap0
```

---

## 封包內容

來源：

```text
10.0.0.1
```

目的：

```text
10.0.0.2
```

協定：

```text
ICMP
```

因此：

```c
ip->protocol = IPPROTO_ICMP;
```

---

## Version 與 IHL

```c
ip->version_ihl = 0x45;
```

代表：

```text
Version = 4
IHL     = 5

5 × 4 = 20 bytes
```

---

# 8. 編譯與執行

## 接收端

```bash
gcc \
-Iinclude \
src/tap.c \
src/ethernet.c \
src/arp.c \
src/arp_table.c \
src/ipv4.c \
-o network

sudo ./network
```

---

## 發送端

```bash
gcc \
-Iinclude \
tests/send_ipv4.c \
src/ethernet.c \
src/arp.c \
src/arp_table.c \
src/ipv4.c \
-o send_ipv4

sudo ./send_ipv4
```

---

# 9. 實測結果

接收端輸出：

```text
[ACCEPT]

Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : 52:54:00:12:34:56
EtherType   : 0x0800

IPv4 Packet
------------------
Version       : 4
Header Length : 20 bytes
TTL           : 64
Protocol      : 1
Source IP     : 10.0.0.1
Destination IP: 10.0.0.2

Frame length: 34 bytes
```

---

# 10. 常見問題（Q&A）

## Q1：為什麼 EtherType 是 0x0800？

Ethernet Header 中的：

```text
EtherType
```

用來告訴接收端 Payload 的格式。

常見值：

| EtherType | 協定   |
| --------- | ---- |
| 0x0800    | IPv4 |
| 0x0806    | ARP  |
| 0x86DD    | IPv6 |

因此：

```text
0x0800
```

代表：

> 接下來的資料是一個 IPv4 Packet。

---

## Q2：為什麼 Version 與 IHL 共用一個 Byte？

IPv4 Header 為了節省空間：

```text
4 Bits
+
4 Bits
=
1 Byte
```

因此：

```text
Version = 高 4 Bits
IHL     = 低 4 Bits
```

---

## Q3：Protocol = 1 代表什麼？

代表：

```text
ICMP
```

也就是：

```text
Ping
Traceroute
Destination Unreachable
Time Exceeded
```

等網路控制訊息所使用的協定。

---

# 今日總結

今天我們正式踏入：

```text
OSI Layer 3
```

並成功完成：

```text
Ethernet
      ↓
IPv4
```

的協定分流與解析。

目前的網路堆疊已經成長為：

```text
TAP Device
      ↑
Ethernet
      ├── ARP
      │     └── ARP Table
      │
      └── IPv4
            └── Header Parser   ← 今天完成
```

換句話說，Ethernet 會透過 EtherType 分流：

```text
EtherType 0x0806 → ARP
EtherType 0x0800 → IPv4
```

我們已經能夠：

* 辨識 IPv4 EtherType
* 解析 IPv4 Header
* 顯示 Source / Destination IP
* 解析 TTL
* 解析 Protocol
* 建立 Layer 2 → Layer 3 Dispatcher

這代表我們終於能夠看懂 Internet 世界中的 IP 封包了。

---

# 下一堂課預告（Day 07）

明天我們會補上 IPv4 Header Checksum，讓自製網路堆疊能夠驗證 IPv4 Header 是否在傳輸過程中被修改。

我們將實作：

```text
ipv4_checksum()
IPv4 Header Checksum Verification
```

這是進入 ICMP / Ping Reply 前的重要準備。
