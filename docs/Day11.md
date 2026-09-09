# Day 11：Routing — 這個封包到底是不是給我的？路由表、子網路遮罩與轉發決策

前十天，我們的網路堆疊大多假設：**「只要封包進來，目的地就是本機（10.0.0.2）」**。
但在真實的網路節點或路由器（Router）中，收到的封包不一定都是給自己的，也可能需要被轉送到其他目的地。

今天我們正式進入網路工程的重要主題 ── **Routing（路由）**。
我們要讓 Network Stack 開始具備「判斷封包下一跳要去哪裡」的能力。

下面這張圖把今天的核心決策整理成一個完整流程：Network Stack 收到 IPv4 Packet 後，會先檢查 Destination IP 是否等於本機 IP；如果是，就交給本機的 ICMP / TCP / UDP 處理（Local Delivery，不扣 TTL）；如果不是，才進入 Router 的 Forwarding Path：扣減 TTL、檢查是否過期，再查詢 Routing Table，決定下一跳或直接丟棄。

![Day11 IPv4 路由決策流程](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day11/Day11_1.png)

這也是 Day11 和前幾天最大的不同：前面我們多半只處理「給自己的封包」，但加入 Routing 之後，Network Stack 開始能判斷封包是否需要被轉送，逐步接近真正路由器的行為。

> 本日範圍說明：Day 11 的重點是 **Routing Lookup / Forwarding Decision（轉發決策）**，也就是「判斷下一跳應該是誰」。目前還沒有真正把封包重新封裝成新的 Ethernet Frame 並 `write()` 送出，因此本文中的「轉發」若未特別註明，指的是「轉發決策」，不是完整 forwarding datapath。

---

# 今日學習目標與成果

完成以下功能與觀念架構建立：
- [x] **建立路由表資料結構（`struct route`）**：支援目標網段（Network）、子網路遮罩（Netmask）、下一跳閘道器（Gateway）。
- [x] **實作路由核心運算（`routing_lookup`）**：使用 `(destination & netmask) == network` 的位元按位與運算（Bitwise AND）判定網段是否命中。
- [x] **支援預設路由（Default Route `0.0.0.0/0`）**：理解 `0.0.0.0/0` 如何作為找不到特定路由時的兜底路徑，並確保特定網段（Specific Route）優先於預設路由。
- [x] **釐清 Routing Table 與 ARP Table 的差異**：理解「L3 決定下一跳 IP」與「L2 決定下一跳 MAC」的職責分工。
- [x] **整合進主程式 `src/tap.c`**：在 IPv4 接收端自動分支：
  - 目的地為本機 ──► 走 Local Delivery，回覆 ICMP Echo Reply。
  - 目的地非本機 ──► 進入 Forwarding Path，扣減 TTL 後查詢路由表，決定是區網直連還是經由 Default Gateway 作為下一跳。
- [x] **實測 Linux 系統路由導流**：在 Linux 主機端動態指派 IP、設定 Next Hop 路由，讓 `ping 8.8.8.8` 的封包進入虛擬網卡並觸發轉發決策。

---

# 核心概念深入剖析

### 1. IP Destination 與 MAC Destination 的根本差異

這是網路封包轉發中最容易混淆、但也最重要的設計之一：

```text
你 (Client)              家用路由器 (Gateway)              Google 伺服器
10.0.0.1 / MAC_A          10.0.0.2 / MAC_G                 8.8.8.8 / MAC_Google
      │                         │                               │
      ├──── 封包從你發出 ──────►│                               │
      │   IP:  SRC=10.0.0.1     │                               │
      │        DST=8.8.8.8      │                               │
      │   MAC: SRC=MAC_A        │                               │
      │        DST=MAC_G        │                               │
      │                         ├──── 路由器往外轉發 ──────────►│
      │                         │   IP:  SRC=10.0.0.1           │ (IP 一路不變)
      │                         │        DST=8.8.8.8            │
      │                         │   MAC: SRC=MAC_G              │ (MAC 每一跳都會更換)
      │                         │        DST=下一跳 MAC          │
```

* **IP Destination（8.8.8.8）**：決定**這個封包最終要送到哪裡**（端到端 End-to-End，沿途路由器通常不改變它）。
* **Ethernet Destination MAC（MAC_G）**：決定**這一跳（Hop）此時此刻要交到身邊誰的手上**（每跨越一台路由器，MAC 標頭就會被重新改寫打包）。

---

### 2. 子網路遮罩（Netmask）的數學本質：Bitwise AND

為什麼只有一個 `&` 運算符，就能辨識是不是同一網段？

* **`&`（Bitwise AND）規則**：兩邊都是 1 才是 1；遇到 0 則通通歸零。
* **遮罩 `255.255.255.0`** 的二進位前 24 個 bit 全是 1，後 8 個 bit 全是 0：
  ```text
  11111111 . 11111111 . 11111111 . 00000000
  ```
* 任何 IP 只要跟它做 `&` 計算：
  * 前面 24 個 bit 遇到 `1`：保留原本的位元，因此可以留下網段部分。
  * 後面 8 個 bit 遇到 `0`：全部清成 0，因此會移除主機編號部分。

#### 計算範例：
```text
  10.0.0.55 的二進位 : 00001010 . 00000000 . 00000000 . 00110111
& 255.255.255.0 遮罩 : 11111111 . 11111111 . 11111111 . 00000000
------------------------------------------------------------------
  AND 運算結果       : 00001010 . 00000000 . 00000000 . 00000000 (10.0.0.0)
```
結果等於網段 `10.0.0.0`，因此可以判定它屬於同一個區域網路。

---

### 3. 預設路由（Default Route `0.0.0.0/0`）的通配奧秘

* 遮罩是 `0.0.0.0`（32 個 bit 全為 0）。
* 任何外網 IP（例如 `8.8.8.8` 或 `140.112.1.1`）跟 `0.0.0.0` 做 `&`，結果都會是 `0.0.0.0`。
* 所以它可以匹配所有 IPv4 位址。在程式中，我們會把它放在特定路由比對之後，作為「找不到更明確路由時」的預設轉發路徑。

---

### 4. Routing Table 與 ARP Table 的差別與協作

| 比較維度 | Routing Table（路由表） | ARP Table（ARP 快取表） |
| :--- | :--- | :--- |
| **屬於哪一層？** | **Layer 3 (網路層)** | **Layer 2 / 2.5 (鏈結層映射)** |
| **輸入** | **最終目的地的 IP**（例如 `8.8.8.8`） | **同網段鄰居或 Gateway 的 IP**（例如 `10.0.0.2`） |
| **輸出** | **下一跳的 Gateway IP**（或 Direct 直連） | **對應的網卡 MAC**（例如 `02:00:00:00:00:01`） |
| **作用範圍** | 跨網段的路徑選擇 | 目前區域網路內的 IP/MAC 對應 |

---

# 今日新增與修改程式碼

### 1. `include/routing.h`：路由表標頭檔

```c
#ifndef ROUTING_H
#define ROUTING_H

#include <stdint.h>

#define ROUTE_TABLE_SIZE 16

struct route {
    uint32_t network; // 目標網段 (Network Byte Order)
    uint32_t netmask; // 子網路遮罩 (Network Byte Order)
    uint32_t gateway; // 下一跳 Gateway IP (0 代表本地直連)
};

void routing_init(void);
int routing_add(uint32_t network, uint32_t netmask, uint32_t gateway);
struct route *routing_lookup(uint32_t destination);
void routing_dump(void);

#endif
```

---

### 2. `src/routing.c`：路由引擎核心實作

```c
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>

#include "routing.h"

static struct route route_table[ROUTE_TABLE_SIZE];
static int route_count = 0;

void routing_init(void)
{
    memset(route_table, 0, sizeof(route_table));
    route_count = 0;
}

int routing_add(uint32_t network, uint32_t netmask, uint32_t gateway)
{
    if (route_count >= ROUTE_TABLE_SIZE) {
        printf("[ROUTING] Table full, cannot add route\n");
        return -1;
    }

    route_table[route_count].network = network;
    route_table[route_count].netmask = netmask;
    route_table[route_count].gateway = gateway;
    route_count++;

    return 0;
}

struct route *routing_lookup(uint32_t destination)
{
    struct route *default_route = NULL;

    for (int i = 0; i < route_count; i++) {
        struct route *r = &route_table[i];

        // 若為 Default Route (0.0.0.0/0)，先記錄當備案
        if (r->netmask == 0 && r->network == 0) {
            default_route = r;
            continue;
        }

        // 核心遮罩運算：(目標 IP & 遮罩) 是否等於 該網段？
        if ((destination & r->netmask) == r->network) {
            return r; // 命中特定網段，優先返回
        }
    }

    // 特定網段都沒命中，最後才走 Default Route
    return default_route;
}

void routing_dump(void)
{
    printf("\n=== Routing Table ===\n");
    printf("%-18s %-18s %-18s\n", "Destination", "Netmask", "Gateway");
    printf("----------------------------------------------------------\n");

    for (int i = 0; i < route_count; i++) {
        struct route *r = &route_table[i];
        char net_str[INET_ADDRSTRLEN];
        char mask_str[INET_ADDRSTRLEN];
        char gw_str[32];

        inet_ntop(AF_INET, &r->network, net_str, sizeof(net_str));
        inet_ntop(AF_INET, &r->netmask, mask_str, sizeof(mask_str));
        
        if (r->gateway == 0) {
            snprintf(gw_str, sizeof(gw_str), "0.0.0.0 (Direct)");
        } else {
            inet_ntop(AF_INET, &r->gateway, gw_str, sizeof(gw_str));
        }

        printf("%-18s %-18s %-18s\n", net_str, mask_str, gw_str);
    }
    printf("=====================\n\n");
}
```

---

### 3. `src/tap.c`：主分流器整合 Local Delivery 與 Routing

```c
                    // 1. 先判斷目的 IP 是不是本機 (10.0.0.2)
                    // Local Delivery 不屬於 Router forwarding path，因此不扣 TTL
                    uint32_t my_ip = *(uint32_t *)LOCAL_IP;

                    if (ip->dst_ip == my_ip) {
                        // 目的 IP 是我：Local Delivery 本地接收
                        printf("[IPv4] Local Delivery (for me)\n");
                        fflush(stdout);

                        switch (ip->protocol) {
                            case IPPROTO_ICMP:
                                icmp_receive(fd, buffer, n);
                                break;
                            default:
                                break;
                        }
                    } else {
                        // 目的 IP 不是我：進入 Router forwarding path
                        printf("[IPv4] Not for me -> Forwarding Path\n");
                        fflush(stdout);

                        // 2. 只有需要轉發的封包才扣 TTL；若歸零則丟棄並送出 Time Exceeded
                        if (ipv4_decrement_ttl(ip) != 0) {
                            printf("[IPv4] TTL Expired\n");
                            icmp_send_time_exceeded(fd, buffer, n);
                            fflush(stdout);
                            break;
                        }

                        // 3. 查詢 Routing Table，做 forwarding decision
                        struct route *r = routing_lookup(ip->dst_ip);
                        if (r == NULL) {
                            printf("[IPv4] No route -> DROP\n");
                            fflush(stdout);
                            break;
                        }

                        if (r->gateway == 0) {
                            printf("[IPv4] Route found: Direct delivery on local network\n");
                        } else {
                            char gw_str[INET_ADDRSTRLEN];
                            inet_ntop(AF_INET, &r->gateway, gw_str, sizeof(gw_str));
                            printf("[IPv4] Route found: Next hop Gateway %s\n", gw_str);
                        }
                        fflush(stdout);
                    }
```

---

# 實測驗收

這次驗證會直接在 Linux 主機設定路由，讓前往外部 IP 的流量進入虛擬網卡 `tap0`，再交給我們的 Network Stack 進行判斷。

實測時可以把 `tap0` 想成 Linux Host 和自製 Network Stack 之間的入口。同樣是從 `tap0` 進來的 IPv4 封包，只要 Destination IP 不同，Network Stack 的反應就會完全不同：如果是給自己的，就走 Local Delivery；如果不是給自己的，就進入 Routing Lookup。

![Day11 tap0 實測情境](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day11/Day11_2.png)

這張圖對應到接下來的驗收操作：`ping 10.0.0.2` 用來觀察本機接收與 ICMP 回覆，而 `ping 8.8.8.8` 搭配 Linux 路由設定，則用來觀察封包被導入 `tap0` 後，如何觸發我們實作的路由轉發決策。
---

### 正確驗收步驟：

#### 步驟 1：啟動網路棧 (Terminal 1)
```bash
sudo ./network
```
啟動後會印出初始化完成的路由表：
```text
=== Routing Table ===
Destination        Netmask            Gateway           
----------------------------------------------------------
10.0.0.0           255.255.255.0      0.0.0.0 (Direct)
0.0.0.0            0.0.0.0            10.0.0.1          
=====================
```

#### 步驟 2：設定 Linux 主機端網卡與導流路由 (Terminal 2)
```bash
# 1. 替 Linux 端的 tap0 設定 10.0.0.1/24，讓雙方位於 10.0.0.0/24 區網
sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null || true

# 2. 告訴 Linux：前往 8.8.8.8 的封包要透過 Gateway 10.0.0.2 轉送
sudo ip route add 8.8.8.8 via 10.0.0.2 dev tap0
```

#### 步驟 3：發送測試封包

---

### 實測情境 1：本機直接接收（Local Delivery）

發送給本機 `10.0.0.2`：
```bash
ping -c 1 10.0.0.2
```

**Terminal 2 (`ping`) 輸出：**
```text
PING 10.0.0.2 (10.0.0.2) 56(84) bytes of data.
64 bytes from 10.0.0.2: icmp_seq=1 ttl=64 time=0.388 ms

--- 10.0.0.2 ping statistics ---
1 packets transmitted, 1 received, 0% packet loss, time 0ms
rtt min/avg/max/mdev = 0.388/0.388/0.388/0.000 ms
```

**Terminal 1 (`./network`) 輸出：**
```text
[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0800

IPv4 Packet
------------------
Version      : 4
Header Length: 20 bytes
TTL          : 64
Protocol     : 1
Checksum     : 0x... (OK)
Source IP    : 10.0.0.1
Destination IP : 10.0.0.2

[IPv4] Local Delivery (for me)
[ICMP] Echo Request received -> Generating Echo Reply
[ICMP] Echo Reply sent (98 bytes)
```
**驗證成功：** 目的 IP 為 `10.0.0.2`（等於 `LOCAL_IP`），判定為本機封包，直接由本機協定棧（ICMP）接收並回覆 Echo Reply。

---

### 實測情境 2：外網路由查詢與轉發決策（Not for me -> Forwarding Path）

發送給外部 IP `8.8.8.8`（經由前面設定的 `via 10.0.0.2` 導流）：
```bash
ping -c 1 8.8.8.8
```

**Terminal 1 (`./network`) 輸出：**
```text
[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : c6:bf:60:33:9d:73
EtherType   : 0x0800

IPv4 Packet
------------------
Version      : 4
Header Length: 20 bytes
TTL          : 64
Protocol     : 1
Checksum     : 0x... (OK)
Source IP    : 10.0.0.1
Destination IP : 8.8.8.8

[IPv4] Not for me -> Forwarding Path
[IPv4] Route found: Next hop Gateway 10.0.0.1
```

**驗證成功：** 目的 IP 為 `8.8.8.8`（不等於 `LOCAL_IP`），因此進入 Forwarding Path 並觸發路由表查詢（Routing Lookup）。透過預設路由 `0.0.0.0/0`，Network Stack 能判斷此封包的下一跳應交由 Gateway `10.0.0.1`。

> 注意：Day 11 到這裡完成的是「下一跳決策」。真正要把封包送出去，還需要查 ARP 取得下一跳 MAC、改寫 Ethernet Header、更新因 TTL 改變而受影響的 IPv4 Checksum，最後再 `write()` 回 TAP；這會在後續傳送路徑逐步補齊。

---

# 目前網路堆疊完整架構演進

```text
                     Network Stack Architecture (Day 11)
 
                              Ethernet (L2)
                                   │
                  ┌────────────────┴────────────────┐
                  │                                 │
             ARP (0x0806)                      IPv4 (0x0800)
                  │                                 │
            ARP Request / Reply               IPv4 Checksum Verify
                  │                                 │
              ARP Table                       TTL Check & Decrement
                                                    │
                                         ┌──────────┴──────────┐
                                         ▼                     ▼
                                    TTL == 0                TTL > 0
                                 (Time Exceeded)               │
                                                               ▼
                                                       Destination Check
                                                               │
                                               ┌───────────────┴───────────────┐
                                               ▼                               ▼
                                         Local Delivery                   Not for me
                                      (ip->dst == LOCAL_IP)                    │
                                               │                               ▼
                                               ▼                         Routing Table
                                      ICMP (Protocol = 1)                 (Lookup Netmask)
                                               │                               │
                                      ┌────────┴────────┐             ┌────────┴────────┐
                                      │                 │             ▼                 ▼
                                 Echo Request      Echo Reply    Route Found         No Route
                                  (Type = 8)       (Type = 0)   (Forward via GW)      (DROP)
```

---

# 下一天：Day 12 預告

到目前為止，我們已經完成了：
* **Layer 2 (Ethernet / ARP)**：找相鄰節點的 MAC。
* **Layer 3 (IPv4 / ICMP / Routing)**：找全球主機的 IP 與路由轉發路徑。

但當一個封包到達了一台主機，這台主機上同時開著：
* 瀏覽器（Chrome）
* 音樂播放器（Spotify）
* 線上遊戲（Discord）

主機怎麼知道這個封包要分給哪一個應用程式？
這就是 **Layer 4：傳輸層（Transport Layer）** 要解決的問題。
**下一天 Day 12** 我們將進入 **UDP (User Datagram Protocol)**，正式引入 **「Port（連接埠）」** 的概念。
