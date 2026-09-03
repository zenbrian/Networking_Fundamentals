# Day 03：Ethernet Broadcast & Unicast —— 建立接收過濾器（Receive Filter）

昨天我們已經成功解析 Ethernet Header，能夠讀取：

* Destination MAC
* Source MAC
* EtherType

然而，真實網卡並不會接收所有經過網路的封包。

今天我們要讓自製的網路堆疊具備真正網卡的第一項核心能力：

```text
Receive Filter
```

也就是：

* 接受（ACCEPT）廣播封包（Broadcast）
* 接受（ACCEPT）發送給自己的單播封包（Unicast to Me）
* 丟棄（DROP）發送給其他設備的單播封包（Unicast to Other）

---

# 今日學習目標

完成以下內容：

* [x] 理解 Ethernet 接收過濾機制
* [x] 建立本機虛擬網卡 MAC Address
* [x] 實作 Broadcast 判斷
* [x] 實作 Unicast to Me 判斷
* [x] 建立 Ethernet Receive Filter
* [x] 驗證 ACCEPT 與 DROP 行為

---

# 1. 核心概念：為什麼需要 Receive Filter？

在真實區域網路中，每秒可能有大量封包在網路上流動。

例如：

```text
PC-A
PC-B
PC-C
PC-D
```

如果所有封包都交給作業系統處理：

```text
Ethernet Frame
      ↓
Linux Kernel
      ↓
CPU 解析
```

即使封包根本不是發送給自己，也會浪費大量 CPU 資源。

因此網卡會先在 Layer 2 建立第一道防線：

```text
Ethernet Frame
      ↓
Receive Filter
      ↓
決定要不要往上層送
```

---

## 1.1 網卡接收決策流程

當 Ethernet Frame 抵達時：

```text
               Ethernet Frame 抵達
                        │
                        ▼
         ┌─────────────────────────────┐
         │ 是廣播封包嗎？               │
         │ ff:ff:ff:ff:ff:ff           │
         └──────────────┬──────────────┘
                        │
            YES         │        NO
             │          │
             ▼          ▼
         [ ACCEPT ]   是否送給我？
                           │
                           ▼
              ┌─────────────────────┐
              │ 目的 MAC == 我的 MAC │
              └──────────┬──────────┘
                         │
              YES        │       NO
               │         │
               ▼         ▼
          [ ACCEPT ]   [ DROP ]
```

這就是大部分網卡最基本的接收邏輯。

---

## 1.2 Broadcast 與 Unicast

### Broadcast（廣播）

廣播 MAC：

```text
ff:ff:ff:ff:ff:ff
```

代表：

> 網段內所有設備都必須接收這個封包。

例如：

```text
ARP Request
```

就是典型的 Broadcast。

---

### Unicast（單播）

單播代表封包只發送給某一台設備。

例如：

```text
02:00:00:00:00:01
```

若目的 MAC 與本機一致：

```text
Destination MAC
        ==
Local MAC
```

則接受。

否則直接丟棄。

---

# 2. 專案架構調整

今天新增一個設定檔：

```text
Networking_Fundamentals
│
├── include
│   ├── config.h
│   └── ethernet.h
│
└── src
    ├── ethernet.c
    └── tap.c
```

---

# 3. 建立本機 MAC 設定

檔案：

```text
include/config.h
```

今天先將我們的虛擬網卡 MAC 固定設定為：

```text
02:00:00:00:00:01
```

---

## 完整程式碼

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

#endif
```

---

## 為什麼使用 02 開頭？

MAC Address 第一個位元組的第二低位元（Locally Administered Bit）為 1 時：

```text
02:xx:xx:xx:xx:xx
```

代表：

```text
Locally Administered Address
```

也就是軟體自行指定的 MAC。

非常適合作為實驗用途。

---

# 4. 更新 Ethernet Header

檔案：

```text
include/ethernet.h
```

新增接收過濾器相關函式：

```c
int ethernet_is_broadcast(const uint8_t *mac);
int ethernet_is_for_me(const uint8_t *mac);
int ethernet_accept_frame(const struct ethernet_hdr *hdr);
```

---

## 函式職責

| 函式                      | 功能      |
| ----------------------- | ------- |
| ethernet_is_broadcast() | 判斷是否為廣播 |
| ethernet_is_for_me()    | 判斷是否為本機 |
| ethernet_accept_frame() | 接收過濾器入口 |

---

# 5. 實作 Ethernet Receive Filter

檔案：

```text
src/ethernet.c
```

---

## 判斷 Broadcast

```c
int ethernet_is_broadcast(const uint8_t *mac)
{
    for (int i = 0; i < ETH_ADDR_LEN; i++) {

        if (mac[i] != 0xff)
            return 0;
    }

    return 1;
}
```

邏輯：

```text
ff ff ff ff ff ff
```

全部都是：

```text
0xff
```

才算廣播。

---

## 判斷是否送給自己

```c
int ethernet_is_for_me(const uint8_t *mac)
{
    return memcmp(
        mac,
        LOCAL_MAC,
        ETH_ADDR_LEN
    ) == 0;
}
```

邏輯：

```text
Destination MAC
        ==
LOCAL_MAC
```

則接受。

---

## 接收過濾器總入口

```c
int ethernet_accept_frame(
    const struct ethernet_hdr *hdr)
{
    if (ethernet_is_broadcast(hdr->dst)) {
        return 1;
    }

    if (ethernet_is_for_me(hdr->dst)) {
        return 1;
    }

    return 0;
}
```

流程：

```text
Broadcast ?
     │
     ├─ YES → ACCEPT
     │
     ▼
For Me ?
     │
     ├─ YES → ACCEPT
     │
     ▼
DROP
```

---

# 6. 整合到接收流程

檔案：

```text
src/tap.c
```

在解析 Ethernet Header 後：

```c
struct ethernet_hdr *eth =
    (struct ethernet_hdr *)buffer;
```

加入：

```c
if (!ethernet_accept_frame(eth)) {

    printf("[DROP] Not for me\n");

    continue;
}

printf("[ACCEPT]\n");

ethernet_print_header(eth);
```

---

此時資料流變成：

```text
Ethernet Frame
        ↓
Ethernet Parser
        ↓
Receive Filter
        ↓
ACCEPT / DROP
```

---

# 7. 編譯與執行

## 編譯

```bash
gcc src/tap.c src/ethernet.c \
    -Iinclude \
    -o ethernet_receiver
```

---

## 執行

```bash
sudo ./ethernet_receiver
```

---

# 8. 三大測試情境

## 測試一：Broadcast

執行：

```bash
ping -I tap0 10.0.0.99
```

Linux 會先送出：

```text
ARP Request
```

其目的 MAC 為：

```text
ff:ff:ff:ff:ff:ff
```

---

程式輸出：

```text
[ACCEPT]

Ethernet Frame
-------------------------
Destination : ff:ff:ff:ff:ff:ff
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0806
Frame length: 42 bytes
```

結果：

```text
Broadcast
     ↓
ACCEPT
```

---

## 測試二：Unicast to Self

建立靜態 ARP：

```bash
sudo ip neigh add \
10.0.0.2 \
lladdr 02:00:00:00:00:01 \
dev tap0
```

測試：

```bash
ping -I tap0 10.0.0.2
```

---

程式輸出：

```text
[ACCEPT]

Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0800
Frame length: 98 bytes
```

結果：

```text
Destination MAC
      ==
LOCAL_MAC
```

成功接受。

---

## 測試三：Unicast to Other

建立另一個 MAC：

```bash
sudo ip neigh add \
10.0.0.3 \
lladdr 02:00:00:00:00:99 \
dev tap0
```

測試：

```bash
ping -I tap0 10.0.0.3
```

---

程式輸出：

```text
[DROP] Not for me
```

結果：

```text
Destination MAC
      !=
LOCAL_MAC
```

因此直接丟棄。

---

# 9. 常見問題（Q&A）

## Q1：為什麼要先檢查 Broadcast？

因為許多重要協定都依賴 Broadcast。

例如：

```text
ARP Request
DHCP Discover
```

若把 Broadcast 全部丟掉：

```text
找不到 MAC
拿不到 IP
```

後續網路功能都無法運作。

---

## Q2：為什麼不用 Source MAC 判斷？

因為接收端關心的是：

```text
這個封包是不是送給我？
```

因此必須檢查：

```text
Destination MAC
```

而不是 Source MAC。

---

# 今日總結

今天我們讓自製網路堆疊具備了真正網卡的重要能力：

```text
Receive Filter
```

資料流正式進化為：

```text
Ethernet Frame
        ↓
Ethernet Parser
        ↓
Receive Filter
        ↓
ACCEPT / DROP
```

目前網路堆疊進度：

```text
Ethernet Frame
      ↓
Ethernet Parser
      ↓
Receive Filter   ← 今天完成
      ↓
ACCEPT / DROP
```

我們已經能夠像真實網卡一樣，決定哪些封包應該被處理，哪些封包應該被直接忽略。

---

# 下一堂課預告（Day 04）

明天將正式進入：

```text
ARP Protocol
(Address Resolution Protocol)
```

我們會第一次主動構造並發送網路協定封包：

```text
Who has 10.0.0.1 ?
Tell 10.0.0.2
```

也就是詢問：

```text
誰的 IP 是 10.0.0.1？
請告訴 10.0.0.2。
```

Day 04 會完成：

* 定義 ARP Header
* 建立本機 IP 設定
* 實作 ARP Request Builder
* 將 ARP 封包封裝進 Ethernet Frame
* 使用 `write()` 從 TAP Device 發送出去
* 使用 `tcpdump` / Wireshark 驗證封包內容

屆時我們的處理流程將從：

```text
Ethernet Frame
      ↓
Ethernet Parser
      ↓
Receive Filter
      ↓
ACCEPT / DROP
```

進一步發展為：

```text
Ethernet Frame
      ↓
Ethernet Parser
      ↓
Receive Filter
      ↓
EtherType = 0x0806
      ↓
ARP Request
```

這代表我們不只會被動接收 Ethernet Frame，也會開始主動產生並送出第一個真正的網路協定封包。
