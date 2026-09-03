# Day 04：ARP Request —— 構造與發送網路世界的「你是誰？」

昨天我們完成了 Ethernet Layer（Layer 2）的接收過濾機制（Receive Filter），讓自製網路堆疊能夠像真正的網卡一樣判斷哪些封包該接收、哪些封包該丟棄。

今天我們將正式進入第一個真正的網路協定：

```text
ARP
(Address Resolution Protocol)
```

ARP 是連接 Layer 2（Ethernet）與 Layer 3（IPv4）的重要橋樑。

完成今天的內容後，我們將能夠：

* 定義 ARP Header 結構
* 手動構造 ARP Request
* 將 ARP 封包封裝進 Ethernet Frame
* 使用 `write()` 發送 Ethernet Frame
* 使用 `tcpdump` 與 Wireshark 驗證封包內容

這也是我們第一次真正主動向網路送出封包。

---

# 今日學習目標

完成以下內容：

* [x] 理解 ARP 的用途與運作流程
* [x] 定義 ARP Header 結構體
* [x] 在 `config.h` 建立本機 IP 位址
* [x] 實作 `arp_build_request()`
* [x] 手動構造 Ethernet + ARP Request
* [x] 使用 `write()` 發送封包
* [x] 使用 `tcpdump` 驗證封包內容
* [x] 使用 Wireshark 驗證 ARP 欄位

---

# 1. 核心概念：為什麼需要 ARP？

在網路世界中存在兩種位址：

| 類型          | 範例                | 所在層     |
| ----------- | ----------------- | ------- |
| IP Address  | 10.0.0.1          | Layer 3 |
| MAC Address | 02:00:00:00:00:01 | Layer 2 |

應用程式與路由表使用的是：

```text
IP Address
```

但 Ethernet 網卡真正傳送資料時使用的是：

```text
MAC Address
```

因此在傳送封包前，必須先完成：

```text
IP Address
      ↓
MAC Address
```

的轉換。

這就是 ARP 的工作。

---

## 1.1 ARP 解決什麼問題？

假設：

```text
PC A
IP : 10.0.0.2
```

想要傳送封包給：

```text
PC B
IP : 10.0.0.1
```

流程如下：

```text
PC A
  │
  ├─ 知道目標 IP
  │
  ├─ 不知道目標 MAC
  │
  ▼
無法建立 Ethernet Header
```

因為 Ethernet Header 必須填寫：

```text
Destination MAC
```

所以必須先詢問：

> 誰的 IP 是 10.0.0.1？

---

## 1.2 ARP Request 運作流程

ARP 採用廣播方式進行查詢。

在送出一般 IPv4 封包之前，主機需要先知道目標 IP 對應到哪一個 MAC Address。當這個對應關係還不存在時，就會先發出 ARP Request，透過 Ethernet Broadcast 問同一個區域網路中的所有設備。

![Day04 ARP Request 查詢流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day04/Day04_1.png)

這張圖可以看到 ARP Request 的核心概念：發送端已經知道目標 IP，但還不知道目標 MAC，因此會把 Ethernet Destination MAC 設成 `ff:ff:ff:ff:ff:ff`，讓區域網路內的所有設備都能收到這個查詢。真正擁有該 IP 的設備，才會在下一步回覆自己的 MAC Address。

流程如下：

```text
PC A
(10.0.0.2)

      │
      │ ARP Request
      ▼

"Who has 10.0.0.1 ?"

      │
      ▼

Broadcast
ff:ff:ff:ff:ff:ff

      │
      ▼

所有設備收到
```

真正擁有：

```text
10.0.0.1
```

的設備會回覆：

```text
ARP Reply

10.0.0.1
=
02:00:00:00:00:99
```

之後 PC A 就能建立 Ethernet Frame。

---

# 2. ARP Packet 結構

ARP Header 固定長度為：

```text
28 Bytes
```

結構如下：

![image](https://www.tsnien.idv.tw/Manager_WebBook/%E6%8F%92%E5%9C%96/chap4/%E5%9C%96%204-7-8.png)

---

## 2.1 各欄位說明

| 欄位    | 長度      | 意義            |
| ----- | ------- | ------------- |
| htype | 2 Bytes | Hardware Type |
| ptype | 2 Bytes | Protocol Type |
| hlen  | 1 Byte  | MAC 長度        |
| plen  | 1 Byte  | IP 長度         |
| oper  | 2 Bytes | ARP 操作碼       |
| sha   | 6 Bytes | Sender MAC    |
| spa   | 4 Bytes | Sender IP     |
| tha   | 6 Bytes | Target MAC    |
| tpa   | 4 Bytes | Target IP     |

---

### ARP Request 固定值

| 欄位    | 數值     |
| ----- | ------ |
| htype | 1      |
| ptype | 0x0800 |
| hlen  | 6      |
| plen  | 4      |
| oper  | 1      |

代表：

```text
Ethernet
+
IPv4
+
ARP Request
```

---

## 2.2 為什麼 Target MAC 要填 0？

因為：

```text
不知道 MAC
      ↓
才需要 ARP
```

因此：

```text
Target MAC

00:00:00:00:00:00
```

代表：

> 我不知道你的 MAC，請你自己告訴我。

---

# 3. 專案架構調整

今天新增 ARP 模組。

```text
Networking_Fundamentals
│
├── include
│   ├── config.h
│   ├── ethernet.h
│   └── arp.h
│
├── src
│   └── arp.c
│
└── test
    └── send_arp.c
```

---

# 4. 建立 ARP Header

檔案：

```text
include/arp.h
```

負責：

* ARP 常數
* ARP Header 定義
* Function Prototype

---

## 完整程式碼

```c
#ifndef ARP_H
#define ARP_H

#include <stdint.h>

#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_packet {

    uint16_t htype;
    uint16_t ptype;

    uint8_t hlen;
    uint8_t plen;

    uint16_t oper;

    uint8_t sha[6];
    uint8_t spa[4];

    uint8_t tha[6];
    uint8_t tpa[4];

} __attribute__((packed));

void arp_build_request(
    struct arp_packet *arp,
    const uint8_t *src_mac,
    const uint8_t *src_ip,
    const uint8_t *target_ip
);

#endif
```

---

## 驗證 Header 大小

```c
printf("%lu\n",
       sizeof(struct arp_packet));
```

輸出：

```text
28
```

表示結構體大小完全符合 RFC 規範。

---

# 5. 建立本機 IP 設定

檔案：

```text
include/config.h
```

從今天開始，課程統一使用以下本機虛擬設備設定：

```c
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

static const uint8_t LOCAL_MAC[6] = {
    0x02,
    0x00,
    0x00,
    0x00,
    0x00,
    0x01
};


static const uint8_t LOCAL_IP[4] = {
    10,
    0,
    0,
    2
};

#endif
```

---

目前我們的虛擬設備資訊：

```text
MAC : 02:00:00:00:00:01
IP  : 10.0.0.2
```

也就是說，接下來送出的 ARP Request 會是：

```text
Who has 10.0.0.1 ?
Tell 10.0.0.2
```

---

# 6. 實作 ARP Request Builder

檔案：

```text
src/arp.c
```

---

## 核心邏輯

設定固定欄位：

```c
arp->htype = htons(1);
arp->ptype = htons(0x0800);

arp->hlen = 6;
arp->plen = 4;

arp->oper = htons(ARP_REQUEST);
```

代表：

```text
Ethernet
+
IPv4
+
ARP Request
```

---

複製來源資訊：

```c
memcpy(arp->sha, src_mac, 6);
memcpy(arp->spa, src_ip, 4);
```

---

設定目標資訊：

```c
memset(arp->tha, 0x00, 6);

memcpy(
    arp->tpa,
    target_ip,
    4
);
```

---

完成後：

```text
Who has 10.0.0.1 ?
Tell 10.0.0.2
```

就已經存在記憶體中了。

---

# 7. 建立發送程式

檔案：

```text
test/send_arp.c
```

今天第一次使用：

```c
write()
```

主動送出 Ethernet Frame。

---

## 封包組裝流程

```text
Ethernet Header
        +
ARP Packet
        ↓
Ethernet Frame
        ↓
write()
        ↓
tap0
```

---

### Ethernet Header

設定：

```text
Destination MAC
=
ff:ff:ff:ff:ff:ff
```

因為 ARP Request 必須廣播。

---

### ARP Payload

查詢：

```text
10.0.0.1
```

對應的 MAC。

---

### Frame 長度

```text
Ethernet Header
14 Bytes

ARP Packet
28 Bytes
```

總長度：

```text
14 + 28 = 42 Bytes
```

---

# 8. 編譯與執行

## 編譯

```bash
cd ~/Networking_Fundamentals

gcc \
test/send_arp.c \
src/arp.c \
-Iinclude \
-o send_arp
```

---

## 開啟監聽

終端機 A：

```bash
sudo tcpdump \
-i tap0 \
-nn \
-e arp
```

---

## 發送 ARP Request

終端機 B：

```bash
sudo ./send_arp
```

輸出：

```text
Sending ARP Request:

Who has 10.0.0.1 ?

Tell 10.0.0.2
```

---

# 9. 驗證結果

## tcpdump

```text
02:00:00:00:00:01 >
ff:ff:ff:ff:ff:ff,

ethertype ARP (0x0806),

Request who-has 10.0.0.1
tell 10.0.0.2
```

---

這代表：

```text
write()
    ↓
tap0
    ↓
Linux Kernel
    ↓
tcpdump
```

已成功收到我們送出的封包。

---

## Wireshark 驗證

| 欄位              | 預期值               | 驗證 |
| --------------- | ----------------- | -- |
| Destination MAC | ff:ff:ff:ff:ff:ff | ✅  |
| Source MAC      | 02:00:00:00:00:01 | ✅  |
| EtherType       | 0x0806            | ✅  |
| Hardware Type   | 1                 | ✅  |
| Protocol Type   | 0x0800            | ✅  |
| Opcode          | 1                 | ✅  |
| Sender IP       | 10.0.0.2          | ✅  |
| Target IP       | 10.0.0.1          | ✅  |

全部欄位與 RFC 826 完全一致。

---

# 10. 常見問題（Q&A）

## Q1：為什麼 Destination MAC 必須是廣播？

因為我們不知道：

```text
10.0.0.1
```

的 MAC。

既然不知道要送給誰，

就只能：

```text
送給所有人
```

因此使用：

```text
ff:ff:ff:ff:ff:ff
```

---

## Q2：為什麼 Target MAC 要填 00:00:00:00:00:00？

因為：

```text
不知道 MAC
      ↓
才發起 ARP
```

若已經知道 MAC，

根本不需要 ARP Request。

---

## Q3：為什麼要使用 htons()？

ARP Header 中的所有多位元組欄位：

```text
htype
ptype
oper
```

都必須使用：

```c
htons()
```

轉換成 Network Byte Order。

否則 Wireshark 會解析錯誤。

---

# 今日總結

今天我們成功完成了第一個真正的網路協定：

```text
ARP Request
```

資料流正式進化為：

```text
Application
      ↓
ARP Builder
      ↓
Ethernet Frame
      ↓
tap0
      ↓
Linux Kernel
```

我們已經具備：

* 建立 ARP Header
* 組裝 Ethernet Frame
* 主動發送封包
* 驗證封包內容

這代表我們不再只是接收資料，而是正式開始與網路互動。

---

# 下一堂課預告（Day 05）

明天我們將實作：

```text
ARP Reply Parser
```

學習接收：

```text
10.0.0.1 is-at
02:00:00:00:00:99
```

並建立第一個真正的核心資料結構：

```text
ARP Table
(IP → MAC Cache)
```

讓我們的網路堆疊開始具備記憶能力，不再每次都需要重新廣播查詢。
