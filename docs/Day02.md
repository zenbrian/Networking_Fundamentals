# Day 02：Ethernet Header —— 把 Bytes 變成 Ethernet Frame

昨天我們已經成功建立 `tap0`，並透過 `read()` 從 Linux Kernel 接收到原始 Ethernet Frame。

今天將正式進入 **OSI Layer 2：Data Link Layer（資料鏈結層）**，學習如何把一串毫無意義的 Raw Bytes，轉換成具有結構的 Ethernet Frame。

完成今天的內容後，我們將具備：

* 解析 Ethernet Header
* 識別封包類型（ARP / IPv4 / IPv6）
* 理解網路位元組序（Network Byte Order）
* 驗證解析結果是否與 `tcpdump` 一致

這是我們開始實作網路協定堆疊的重要里程碑。

---

# 今日學習目標

完成以下內容：

* [x] 理解 Ethernet II Frame 結構
* [x] 建立 Ethernet Header 結構體
* [x] 解析 Destination MAC
* [x] 解析 Source MAC
* [x] 解析 EtherType
* [x] 理解 Big-Endian 與 Little-Endian
* [x] 驗證解析結果與 `tcpdump` 完全一致

---

# 1. 核心概念：Ethernet Frame

當 Linux Kernel 將資料送到 TAP Device 時，送來的並不是 IP 封包，而是一個完整的 Ethernet Frame。

結構如下：

```text
┌──────────────────────┬──────────────────────┬─────────────┐
│ Destination MAC      │ Source MAC           │ EtherType   │
│       6 Bytes        │       6 Bytes        │   2 Bytes   │
└──────────────────────┴──────────────────────┴─────────────┘
                             ↓
                          Payload
```

Ethernet Header 固定長度為：

```text
6 + 6 + 2 = 14 Bytes
```

因此：

```text
Ethernet Frame
      ↓
Ethernet Header (14 Bytes)
      ↓
Payload (46 ~ 1500 Bytes)
```

---

## 1.1 Destination MAC

前 6 Bytes 表示封包的接收者。

例如：

```text
ff ff ff ff ff ff
```

代表：

```text
ff:ff:ff:ff:ff:ff
```

這是 Ethernet 廣播位址（Broadcast Address）。

表示：

> 網段中的所有設備都應該接收這個封包。

ARP Request 幾乎都會使用廣播位址。

---

## 1.2 Source MAC

接下來的 6 Bytes 表示封包發送者。

例如：

```text
c6 bf 60 33 9d 73
```

解析後：

```text
c6:bf:60:33:9d:73
```

這就是送出封包的網卡 MAC Address。

---

## 1.3 EtherType

最後 2 Bytes 用來告知：

> Payload 裡面裝的是哪種網路協定。

常見類型如下：

| EtherType | 協定   |
| --------- | ---- |
| `0x0800`  | IPv4 |
| `0x0806`  | ARP  |
| `0x86DD`  | IPv6 |

例如：

```text
08 06
```

解析後：

```text
0x0806
```

表示：

```text
ARP Protocol
```

---

# 2. 網路位元組序（Byte Order）

在解析 EtherType 前，我們必須先理解一個重要概念：

```text
Byte Order
```

也就是資料在記憶體中的排列方式。

---

## 2.1 Little-Endian（x86 CPU）

大部分桌上型電腦使用 x86 CPU。

x86 採用：

```text
Little Endian
```

例如：

```text
0x0806
```

在記憶體中的實際排列為：

```text
06 08
```

最低位元組放在前面。

---

## 2.2 Big-Endian（Network Order）

網路協定統一規定使用：

```text
Big Endian
```

又稱：

```text
Network Byte Order
```

因此：

```text
0x0806
```

在封包中會以：

```text
08 06
```

的形式傳送。

---

## 2.3 為什麼需要 ntohs()？

當我們收到：

```text
08 06
```

時，

x86 CPU 會將它解讀成：

```text
0x0608
```

導致協定解析錯誤。

因此必須進行轉換：

```c
ntohs()
```

全名：

```text
Network To Host Short
```

使用方式：

```c
uint16_t type =
    ntohs(hdr->ethertype);
```

---

## 2.4 為什麼需要 htons()？

未來我們自己組裝封包時：

```c
hdr->ethertype = htons(ETHERTYPE_ARP);
```

Linux 才能正確解析：

```text
08 06
```

而不是：

```text
06 08
```

---

# 3. 專案檔案結構

今天開始將 Ethernet 相關功能獨立成模組。

專案結構如下：

```text
Networking_Fundamentals
│
├── include
│   └── ethernet.h
│
└── src
    ├── ethernet.c
    └── tap.c
```

---

# 4. ethernet.h

此檔案負責定義：

* Ethernet Header
* EtherType 常數
* 函式介面

```c
#ifndef ETHERNET_H
#define ETHERNET_H

#include <stdint.h>

#define ETH_ADDR_LEN 6
#define ETH_HEADER_LEN 14

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806
#define ETHERTYPE_IPV6 0x86DD

struct ethernet_hdr {
    uint8_t dst[ETH_ADDR_LEN];
    uint8_t src[ETH_ADDR_LEN];
    uint16_t ethertype;
} __attribute__((packed));

void ethernet_print_mac(const uint8_t *mac);
void ethernet_print_header(const struct ethernet_hdr *hdr);

#endif
```

---

# 5. ethernet.c

負責：

* 格式化輸出 MAC Address
* 顯示 Ethernet Header 內容

```c
#include <stdio.h>
#include <arpa/inet.h>
#include "ethernet.h"

void ethernet_print_mac(const uint8_t *mac)
{
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
}

void ethernet_print_header(
    const struct ethernet_hdr *hdr)
{
    printf("Ethernet Frame\n");
    printf("-------------------------\n");

    printf("Destination : ");
    ethernet_print_mac(hdr->dst);
    printf("\n");

    printf("Source      : ");
    ethernet_print_mac(hdr->src);
    printf("\n");

    printf("EtherType   : 0x%04x\n",
           ntohs(hdr->ethertype));
}
```

---

# 6. tap.c

今天最大的改變是：

我們不再只是印出 Raw Bytes。

而是將資料直接轉型成：

```c
struct ethernet_hdr
```

進行結構化解析。

核心程式碼：

```c
struct ethernet_hdr *eth =
    (struct ethernet_hdr *)buffer;
```

此時：

```text
buffer
  ↓
Ethernet Header
```

我們就能透過：

```c
eth->dst
eth->src
eth->ethertype
```

直接存取各個欄位。

---

# 7. 編譯與執行

編譯：

```bash
cd ~/Networking_Fundamentals

gcc src/tap.c src/ethernet.c \
    -Iinclude \
    -o network
```

執行：

```bash
sudo ./network
```

另一個終端機：

```bash
ping -I tap0 10.0.0.1
```

此時 Linux 會送出 ARP Request。

我們的程式便能收到並解析完整 Ethernet Frame。

---

# 8. 驗證解析結果

程式輸出：
![image](https://hackmd.io/_uploads/ryHtPF0wGl.png)

---

同時使用：

```bash
sudo tcpdump -i tap0 -e
```

抓取封包：

```text
c6:bf:60:33:9d:73 >
ff:ff:ff:ff:ff:ff,
ethertype ARP (0x0806),
length 42
```

---

# 9. 今日問答（Q&A）

## Q1：為什麼要執行 `ping -I tap0 10.0.0.1`？

因為：

```c
read(fd, buffer, sizeof(buffer));
```

會阻塞等待封包。

若沒有流量經過 `tap0`：

```text
程式
 ↓
等待
 ↓
等待
 ↓
持續等待
```

什麼都不會發生。

---

執行：

```bash
ping -I tap0 10.0.0.1
```

後，

Linux 會先送出：

```text
ARP Request
```

因此可以快速產生測試流量。

---

## Q2：既然已經有自己的解析器，為什麼還要用 tcpdump？

原因有兩個：

### 原因一：獨立驗證

自己寫的程式可能：

* Offset 算錯
* 結構體定義錯誤
* Byte Order 處理錯誤

而不自知。

`tcpdump` 是業界標準工具。

只有兩邊結果一致，才能確認程式正確。

---

### 原因二：驗證封包真的進入 Kernel

若：

```text
自己程式看得到
tcpdump 看不到
```

代表程式可能有問題。

若：

```text
tcpdump 看得到
自己程式看不到
```

代表解析器有問題。

兩者互相驗證，才能縮小除錯範圍。

---

# 10. ARP 封包實測比對

## tcpdump

```text
10:36:05.156750
c6:bf:60:33:9d:73 >
ff:ff:ff:ff:ff:ff,
ethertype ARP (0x0806),
length 42
```

---

## 我們的程式

```text
Ethernet Frame
-------------------------
Destination : ff:ff:ff:ff:ff:ff
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0806
Frame length: 42 bytes
```

---

## 比對結果

| 欄位              | tcpdump           | 我們的程式             | 結果 |
| --------------- | ----------------- | ----------------- | -- |
| Destination MAC | ff:ff:ff:ff:ff:ff | ff:ff:ff:ff:ff:ff | ✅  |
| Source MAC      | c6:bf:60:33:9d:73 | c6:bf:60:33:9d:73 | ✅  |
| EtherType       | ARP (0x0806)      | 0x0806            | ✅  |
| Length          | 42                | 42                | ✅  |

完全一致。

代表我們的 Ethernet Parser 已經正確運作。

---


# 下一堂課預告（Day 03）

明天我們會繼續停留在 Ethernet Layer，實作真正網卡都具備的第一道防線：

```text
Receive Filter
```

目前我們已經能解析 Ethernet Header：

```text
Ethernet Frame
      ↓
Destination MAC
Source MAC
EtherType
```

但真實網卡不會把所有封包都交給上層處理。

下一堂課會讓我們的自製網路堆疊能夠根據 Destination MAC 做出判斷：

```text
Broadcast        → ACCEPT
Unicast to Me    → ACCEPT
Unicast to Other → DROP
```

屆時我們的 Ethernet Layer 將從：

```text
Ethernet Frame
      ↓
Ethernet Parser
```

進一步發展為：

```text
Ethernet Frame
      ↓
Ethernet Parser
      ↓
Receive Filter
      ↓
ACCEPT / DROP
```

完成這一步後，我們才會在 Day 04 正式進入 ARP Protocol，開始構造並發送第一個真正的網路協定封包。
