# Day 5：ARP Reply 與 ARP Table 快取記憶庫

昨天我們完成了發送 **ARP Request** (`Who has 10.0.0.1? Tell 10.0.0.2`)。
今天我們完成了 ARP 協定的另一半：接收與解析 **ARP Reply**，並建立了網路堆疊的第一個快取記憶庫 ── **ARP Table**，同時具備自動回應 **ARP Request** 的能力。

```text
ARP Request
      ↓
收到 ARP Reply ──► 解析 Sender IP/MAC ──► 寫入與查詢 ARP Table
      ↓
收到 ARP Request ──► 判斷是否為本機 IP ──► 自動組裝並發送 ARP Reply
```

---

# 今日學習目標與成果

完成以下完整流程：

```text
10.0.0.1  ────►  52:54:00:12:34:56  (自動學習並存入 ARP Table)
```

驗收項目：
- [x] 建立 `arp_table.h` 與 `arp_table.c` 結構與實作
- [x] 支援 `arp_table_init()`, `arp_table_insert()`, `arp_table_lookup()`, `arp_table_dump()`
- [x] 實作 `arp_handle_reply()` 與 `arp_handle_request()` 自動回覆機制
- [x] 升級 `arp_receive()`：同時支援 `ARP_REQUEST` (Opcode=1) 與 `ARP_REPLY` (Opcode=2)
- [x] 整合 Ethernet Layer 的 **EtherType 分流器 (Dispatcher)**
- [x] 實作 `tap0` 介面自動 `UP` 機制與 Raw Socket 測試腳本
- [x] 驗證成功解析 ARP Reply 並寫入與印出 ARP Table
- [x] 完成專案架構重構（將 Core 核心與 Test 工具分離）

---

# 今日更新程式碼

### 1. `include/arp_table.h`
定義 ARP Table Entry 資料結構與函式介面：

```c
#ifndef ARP_TABLE_H
#define ARP_TABLE_H

#include <stdint.h>

#define ARP_TABLE_SIZE 32

struct arp_entry {
    uint8_t ip[4];
    uint8_t mac[6];
    int valid;
};

extern struct arp_entry arp_table[ARP_TABLE_SIZE];

void arp_table_init(void);
int arp_table_insert(const uint8_t *ip, const uint8_t *mac);
struct arp_entry *arp_table_lookup(const uint8_t *ip);
void arp_table_dump(void);

#endif
```


### 2. `src/arp_table.c`
實作 ARP Table 的記憶體初始化、新增/更新、查詢與格式化傾印：

```c
#include <stdio.h>
#include <string.h>

#include "arp_table.h"

struct arp_entry arp_table[ARP_TABLE_SIZE];

void arp_table_init(void)
{
    memset(arp_table, 0, sizeof(arp_table));
}

int arp_table_insert(const uint8_t *ip, const uint8_t *mac)
{
    // 若 IP 已存在則更新 MAC
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && memcmp(arp_table[i].ip, ip, 4) == 0) {
            memcpy(arp_table[i].mac, mac, 6);
            return 0;
        }
    }

    // 寫入第一個空位
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            memcpy(arp_table[i].ip, ip, 4);
            memcpy(arp_table[i].mac, mac, 6);
            arp_table[i].valid = 1;
            return 0;
        }
    }

    return -1; // 表滿
}

struct arp_entry *arp_table_lookup(const uint8_t *ip)
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && memcmp(arp_table[i].ip, ip, 4) == 0) {
            return &arp_table[i];
        }
    }
    return NULL;
}

void arp_table_dump(void)
{
    printf("\nARP Table\n=========================\n");
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid) {
            printf("%u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                   arp_table[i].ip[0], arp_table[i].ip[1],
                   arp_table[i].ip[2], arp_table[i].ip[3],
                   arp_table[i].mac[0], arp_table[i].mac[1],
                   arp_table[i].mac[2], arp_table[i].mac[3],
                   arp_table[i].mac[4], arp_table[i].mac[5]);
        }
    }
    printf("\n");
    fflush(stdout);
}
```

### 3. `include/arp.h` & `src/arp.c` (ARP 處理核心)

```c
// include/arp.h
#ifndef ARP_H
#define ARP_H

#include <stdint.h>
#include <stddef.h>

#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t  hlen;
    uint8_t  plen;
    uint16_t oper;

    uint8_t  sha[6];
    uint8_t  spa[4];

    uint8_t  tha[6];
    uint8_t  tpa[4];
} __attribute__((packed));

void arp_build_request(
    struct arp_packet *arp,
    const uint8_t *src_mac,
    const uint8_t *src_ip,
    const uint8_t *target_ip
);

void arp_handle_reply(const struct arp_packet *arp);
void arp_handle_request(int fd, const struct arp_packet *arp);
void arp_receive(int fd, const uint8_t *payload, size_t len);

#endif
```

```c
// src/arp.c
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "config.h"
#include "ethernet.h"
#include "arp.h"
#include "arp_table.h"

void arp_handle_reply(const struct arp_packet *arp)
{
    arp_table_insert(arp->spa, arp->sha);
    printf("[ARP] Reply Received\n");
    fflush(stdout);
    arp_table_dump();
}

void arp_handle_request(int fd, const struct arp_packet *arp)
{
    // 只回應詢問 LOCAL_IP (10.0.0.2) 的 ARP Request
    if (memcmp(arp->tpa, LOCAL_IP, 4) != 0) {
        return;
    }

    printf("[ARP] Request for %u.%u.%u.%u received -> Generating ARP Reply\n",
           arp->tpa[0], arp->tpa[1], arp->tpa[2], arp->tpa[3]);
    fflush(stdout);

    // 順便學習請求者的 IP/MAC
    arp_table_insert(arp->spa, arp->sha);

    // 組裝完整的 Ethernet + ARP Reply Frame (14 + 28 = 42 bytes)
    uint8_t frame[ETH_HEADER_LEN + sizeof(struct arp_packet)];
    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct arp_packet *reply = (struct arp_packet *)(frame + ETH_HEADER_LEN);

    // 1. Ethernet Header
    memcpy(eth->dst, arp->sha, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_ARP);

    // 2. ARP Header & Payload
    reply->htype = htons(1);
    reply->ptype = htons(0x0800);
    reply->hlen = 6;
    reply->plen = 4;
    reply->oper = htons(ARP_REPLY);

    memcpy(reply->sha, LOCAL_MAC, 6);
    memcpy(reply->spa, LOCAL_IP, 4);
    memcpy(reply->tha, arp->sha, 6);
    memcpy(reply->tpa, arp->spa, 4);

    // 3. 發送回 TAP 虛擬網卡
    ssize_t sent = write(fd, frame, sizeof(frame));
    if (sent < 0) {
        perror("[ARP] write failed");
    } else {
        printf("[ARP] Reply sent (%ld bytes)\n", sent);
        fflush(stdout);
    }
}

void arp_receive(int fd, const uint8_t *payload, size_t len)
{
    if (len < sizeof(struct arp_packet))
        return;

    const struct arp_packet *arp = (const struct arp_packet *)payload;
    uint16_t opcode = ntohs(arp->oper);

    if (opcode == ARP_REQUEST) {
        arp_handle_request(fd, arp);
    } else if (opcode == ARP_REPLY) {
        arp_handle_reply(arp);
    }
}
```

### 4. `src/tap.c` (主程式與 EtherType 分流整合)

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "ethernet.h"
#include "arp.h"
#include "arp_table.h"

int tun_alloc(char *dev)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (dev && *dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        exit(1);
    }

    strcpy(dev, ifr.ifr_name);

    // 自動將 TAP 網卡設置為 UP | RUNNING 狀態
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock >= 0) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
        if (ioctl(sock, SIOCGIFFLAGS, &ifr) >= 0) {
            ifr.ifr_flags |= (IFF_UP | IFF_RUNNING);
            ioctl(sock, SIOCSIFFLAGS, &ifr);
        }
        close(sock);
    }

    return fd;
}

int main()
{
    char dev[IFNAMSIZ] = "tap0";
    unsigned char buffer[2048];

    int fd = tun_alloc(dev);
    arp_table_init();

    printf("TAP device: %s (UP)\n", dev);
    printf("Ethernet header size: %lu bytes\n", sizeof(struct ethernet_hdr));
    printf("Waiting for Ethernet frame...\n");
    fflush(stdout);

    while (1) {
        int n = read(fd, buffer, sizeof(buffer));

        if (n < 0) {
            perror("read");
            break;
        }

        if (n < ETH_HEADER_LEN) {
            printf("Invalid Ethernet frame\n");
            fflush(stdout);
            continue;
        }

        struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;

        if (!ethernet_accept_frame(eth)) {
            printf("[DROP] Not for me\n");
            fflush(stdout);
            continue;
        }

        printf("[ACCEPT]\n");
        ethernet_print_header(eth);

        // Ethernet EtherType 分流器 (Dispatcher)
        const uint8_t *payload = buffer + ETH_HEADER_LEN;
        size_t payload_len = n - ETH_HEADER_LEN;

        switch (ntohs(eth->ethertype)) {
            case ETHERTYPE_ARP:
                arp_receive(fd, payload, payload_len);
                break;

            default:
                printf("Unknown EtherType: 0x%04x\n", ntohs(eth->ethertype));
                break;
        }

        printf("Frame length: %d bytes\n\n", n);
        fflush(stdout);
    }

    close(fd);
    return 0;
}
```

### 5. `tests/send_arp_reply.c` (Raw Socket 測試發送端)

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/if_ether.h>
#include <netpacket/packet.h>

#include "config.h"
#include "ethernet.h"
#include "arp.h"

int main()
{
    int sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (sockfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_ifindex = if_nametoindex("tap0");
    sll.sll_protocol = htons(ETH_P_ALL);

    if (sll.sll_ifindex == 0) {
        perror("if_nametoindex tap0 (請確保 ./network 正在執行中！)");
        close(sockfd);
        return 1;
    }

    unsigned char frame[64];
    memset(frame, 0, sizeof(frame));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HEADER_LEN);

    uint8_t sender_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t sender_ip[4]  = {10, 0, 0, 1};

    memcpy(eth->dst, LOCAL_MAC, ETH_ADDR_LEN);
    memcpy(eth->src, sender_mac, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->htype = htons(1);
    arp->ptype = htons(0x0800);
    arp->hlen  = 6;
    arp->plen  = 4;
    arp->oper  = htons(ARP_REPLY);

    memcpy(arp->sha, sender_mac, 6);
    memcpy(arp->spa, sender_ip, 4);
    memcpy(arp->tha, LOCAL_MAC, 6);
    memcpy(arp->tpa, LOCAL_IP, 4);

    int len = ETH_HEADER_LEN + sizeof(struct arp_packet);

    printf("Sending ARP Reply via Raw Socket: 10.0.0.1 is at 52:54:00:12:34:56...\n");

    if (sendto(sockfd, frame, len, 0, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        perror("sendto");
    } else {
        printf("Sent %d bytes ARP Reply frame successfully!\n", len);
    }

    close(sockfd);
    return 0;
}
```

---

# 今日問答與核心觀念整理 (Q&A Summary)

### Q1: ARP 標頭中的 `Opcode` 是什麼？
* **答**：`Opcode` (Operation Code，操作碼) 是 ARP 標頭中 16-bit 的欄位，用來告訴接收端這個封包是在「問問題」還是在「回答問題」。
  * `Opcode = 1` (`ARP_REQUEST`) ➔ 廣播詢問「誰有這個 IP？請告訴我 MAC」。
  * `Opcode = 2` (`ARP_REPLY`) ➔ 單播回應「這個 IP 在這個 MAC」。

---

### Q2: `ETHERTYPE_ARP` (`0x0806`) 與 `ETHERTYPE_IPV4` (`0x0800`) 的用途分別是什麼？
* **答**：
  * `ETHERTYPE_ARP` (`0x0806`)：Layer 2 與 Layer 3 之間的「譯者與尋人廣播」，用途是將 IP 位址翻譯為 MAC 位址並建立 ARP Table。
  * `ETHERTYPE_IPV4` (`0x0800`)：正式的 Layer 3 網路層數據傳輸通道，裝載真正的 IPv4 標頭與高層數據（如 TCP/UDP/Ping）。

---

### Q3: 沒找到 MAC 位址前要先傳 ARP，但如果 ARP Table 已經有 MAC 位址，就可以直接傳 IPv4 嗎？
* **答**：**完全正確！**
  * **Cache Hit** (ARP Table 有紀錄) ➔ 直接封裝 Ethernet Header (Destination MAC = 查到的 MAC, EtherType = 0x0800)，立即送出 IPv4 封包。
  * **Cache Miss** (ARP Table 無紀錄) ➔ 暫存 IPv4 封包，並向區域網路發送 ARP Request 廣播，收到 ARP Reply 寫入 Table 後再把 IPv4 封包送出。
  * ARP Cache 機制能有效避免廣播風暴並極致提升傳輸效率。

---

### Q4: ARP Dispatcher (分流器) 要寫在哪裡？
* **答**：分為兩層分流：
  1. **第一層（Ethernet 分流）**：寫在 `src/tap.c` 的 `while(1)` 迴圈中，依據 `ntohs(eth->ethertype)` 是否為 `0x0806` 分流至 `arp_receive()`。
  2. **第二層（ARP 內部分流）**：寫在 `src/arp.c` 的 `arp_receive()` 函式中：
     - 若為 `ARP_REQUEST` (Opcode = 1) ➔ 分流至 `arp_handle_request()`（判斷是否問自己並自動回送 ARP Reply）。
     - 若為 `ARP_REPLY` (Opcode = 2) ➔ 分流至 `arp_handle_reply()`（寫入 ARP Table）。

---

### Q5: 執行 `./send_arp_reply` 時遇到 `TUNSETIFF: Device or resource busy` 該如何解決？
* **答**：因為 `/dev/net/tun` 裝置同時只能被一個行程獨佔鎖定（即運作中的 `network`）。解決方案是將發送端改用 Linux **Raw Socket (`socket(AF_PACKET, SOCK_RAW, ...)` )**，直接經由 `tap0` 介面注入封包，無需重複開啟 `/dev/net/tun`。

---

### Q6: 執行 `./send_arp_reply` 時遇到 `sendto: Network is down` 該如何解決？
* **答**：因為剛建立的 `tap0` 網卡狀態預設為 `DOWN`。可透過命令 `sudo ip link set tap0 up` 切換為 UP，或在 `src/tap.c` 的 `tun_alloc()` 內呼叫 `ioctl(SIOCSIFFLAGS)` 自動將 `IFF_UP | IFF_RUNNING` 狀態拉起。

---

### Q7: 為什麼 `network` 執行時會收到很多 `[DROP] Not for me`？
* **答**：當 `tap0` 切換為 `UP` 狀態時，Linux 作業系統背景會自動經由該網卡發送維護封包（如 IPv6 NDP 多播封包，Destination MAC 為 `33:33:xx:xx:xx:xx`）。因為這些封包既不是全廣播 (`ff:ff:ff:ff:ff:ff`) 也不是我們的本機 MAC (`02:00:00:00:00:01`)，因此被我們的 `ethernet_accept_frame()` 守門員正確判定為 `Not for me` 並過濾丟棄。這證明我們的 Layer 2 MAC 過濾器完全正常運作！

---

### Q8: 我們有測試 `arp_handle_reply()` 嗎？
* **答**：**有！** 當視窗二發送 ARP Reply 時，視窗一印出的 `[ARP] Reply Received` 與底下的 `ARP Table (10.0.0.1 -> 52:54:00:12:34:56)` 就是 `arp_handle_reply()` 與 `arp_table_dump()` 100% 成功執行的結果。

---

# 實測驗收結果

### 執行命令
**視窗一（執行主程式）**：
```bash
gcc -Iinclude src/tap.c src/ethernet.c src/arp.c src/arp_table.c -o network
sudo ./network
```

**視窗二（執行測試發送端）**：
```bash
gcc -Iinclude tests/send_arp_reply.c src/ethernet.c src/arp.c src/arp_table.c -o send_arp_reply
sudo ./send_arp_reply
```

### 視窗一接收端終極實測日誌
```text
TAP device: tap0 (UP)
Ethernet header size: 14 bytes
Waiting for Ethernet frame...

[DROP] Not for me
[ACCEPT]
Ethernet Frame
-------------------------
Destination : 02:00:00:00:00:01
Source      : 52:54:00:12:34:56
EtherType   : 0x0806

[ARP] Reply Received

ARP Table
=========================
10.0.0.1 -> 52:54:00:12:34:56

Frame length: 42 bytes
```

---

# 下一天：Day 6 預告

明天我們將正式離開 Layer 2，跨入 **Layer 3 (Network Layer)**：

```text
Ethernet
    │
    ├── ARP (0x0806)  ───► ARP Table / ARP Reply (已完成)
    │
    └── IPv4 (0x0800) ───► 解析第一個真正的 IP 封包
```

我們會解析 IPv4 標頭欄位：`Version`, `IHL`, `TTL`, `Protocol`, `Source IP`, `Destination IP`，並建立完整的 IP 處理模組！
