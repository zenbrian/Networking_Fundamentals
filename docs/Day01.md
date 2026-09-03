# Day 01：TAP Device —— 建立我們的「虛擬網路線」

---

# 今日學習目標

完成以下內容：

* [x] 建立並啟用 `tap0`
* [x] 使用 C 語言開啟 `/dev/net/tun`
* [x] 將程式綁定至 `tap0`
* [x] 透過 `read()` 接收 Linux 核心送出的 Ethernet Frame
* [x] 理解 Ethernet Frame 的基本結構

  * 目的 MAC Address
  * 來源 MAC Address
  * EtherType

---

# 1. 核心概念：什麼是 TAP Device？

在真正開始撰寫網路協定之前，我們需要先建立一條能夠讓 Linux 核心與我們的程式互相傳送封包的「虛擬網路線」。

這條虛擬網路線，就是 **TAP Device**。

![alt text](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day01/Day01_1.png)

---

## 1.1 實體網卡與 TAP 虛擬網卡

### 實體網卡（Physical NIC）

實體網卡負責：

1. 接收網路線上的電氣訊號或光訊號
2. 轉換成 Ethernet Frame
3. 將資料交給 Linux 核心

流程如下：

```text
網路線
   ↓
實體網卡
   ↓
Linux Kernel
```

---

### TAP 虛擬網卡

TAP 網卡不存在任何實體硬體。

它是 Linux 核心建立出來的虛擬網路介面，而介面的另一端直接連接到我們的程式。

流程如下：

```text
Linux Kernel
      ↓
    tap0
      ↓
   我們的程式
```

因此：

* 核心送出的封包會被程式接收
* 程式送出的封包會被核心接收

從核心的角度來看，我們的程式就像是一張真正存在的網路卡。

---

## 1.2 TUN 與 TAP 的差異

| 裝置類型    | 提供給程式的資料層級               | 內容                           | 適用場景          |
| ------- | ------------------------ | ---------------------------- | ------------- |
| **TUN** | Layer 3（Network Layer）   | IP Packet                    | VPN、IP Tunnel |
| **TAP** | Layer 2（Data Link Layer） | Ethernet Frame（含 MAC Header） | 虛擬機、自製網路協定堆疊  |

---

### 為什麼本課程選擇 TAP？

因為我們要親手實作完整網路堆疊：

```text
Application
      ↓
TCP / UDP
      ↓
IPv4
      ↓
ARP
      ↓
Ethernet
```

若使用 TUN：

```text
IP Packet
   ↓
程式
```

Ethernet 與 ARP 都會被 Linux 核心隱藏。

而使用 TAP：

```text
Ethernet Frame
      ↓
程式
```

我們可以從 Layer 2 開始實作，真正理解整個網路協定的運作方式。

---

# 2. 建立 TAP 網卡

建立一張名為 `tap0` 的虛擬網卡。

```bash
# 建立 TAP 裝置
sudo ip tuntap add dev tap0 mode tap

# 指定 IP 位址
sudo ip addr add 10.0.0.1/24 dev tap0

# 啟用網卡
sudo ip link set tap0 up

# 檢查狀態
ip addr show tap0
```

建立完成後，可以看到：

![alt text](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day01/Day01_2.png)

此時 Linux 已經認為系統中存在一張名為 `tap0` 的網路卡。

---

# 3. 使用 C 語言開啟 TAP Device

接下來撰寫程式，與 Linux 核心建立連線。

檔案：

```text
src/tap.c
```

---

## 完整程式碼

```c
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/if_tun.h>
#include <net/if.h>

int tun_alloc(char *dev)
{
    struct ifreq ifr;
    int fd;

    // 開啟 TUN/TAP 核心驅動
    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open /dev/net/tun");
        exit(1);
    }

    memset(&ifr, 0, sizeof(ifr));

    // TAP 模式 + 不附加額外資訊
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;

    if (dev && *dev) {
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);
    }

    // 綁定 tap0
    if (ioctl(fd, TUNSETIFF, (void *)&ifr) < 0) {
        perror("ioctl TUNSETIFF");
        close(fd);
        exit(1);
    }

    strcpy(dev, ifr.ifr_name);
    return fd;
}

int main()
{
    char dev[IFNAMSIZ] = "tap0";
    unsigned char buffer[2048];

    int fd = tun_alloc(dev);

    printf("TAP device: %s\n", dev);
    printf("Waiting for Ethernet frame...\n");

    while (1) {

        int n = read(fd, buffer, sizeof(buffer));

        if (n < 0) {
            perror("read");
            break;
        }

        printf("Received %d bytes\n", n);

        printf("First bytes: ");

        for (int i = 0; i < n && i < 32; i++) {
            printf("%02x ", buffer[i]);
        }

        printf("\n");
    }

    close(fd);
    return 0;
}
```

---

## 編譯與執行

```bash
gcc src/tap.c -o tap

sudo ./tap
```

執行後：

![alt text](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day01/Day01_3.png)

此時程式已經進入封包接收狀態。

---

# 4. 產生測試封包

打開另一個終端機：

```bash
ping 10.0.0.2
```

此時 Linux 核心會嘗試尋找：

```text
10.0.0.2 對應的 MAC Address
```

因此送出 ARP Request。

我們的程式便能接收到第一個 Ethernet Frame。
![alt text](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day01/Day01_4.png)


---

# 5. 常見問題（Q&A）

## Q1：為什麼開啟的是 `/dev/net/tun`？

為什麼不是：

```text
/dev/tap0
```

或

```text
/dev/tap
```

？

原因是 Linux 將 TUN 與 TAP 統一交由同一個核心驅動管理：

```text
tun.ko
```

所有程式都先開啟：

```text
/dev/net/tun
```

接著再透過：

```c
ioctl(fd, TUNSETIFF, &ifr);
```

指定：

```c
IFF_TAP
```

告訴核心：

> 我要使用 TAP 模式，並綁定到 tap0。

---

## Q2：為什麼測試要 ping 10.0.0.2？

因為：

```text
10.0.0.1
```

是 `tap0` 自己的 IP。

若執行：

```bash
ping 10.0.0.1
```

Linux 會直接在核心內部處理。

```text
Ping
 ↓
Kernel
 ↓
回覆
```

完全不會經過 `tap0`。

因此我們的程式收不到任何封包。

---

當執行：

```bash
ping 10.0.0.2
```

時：

```text
Linux Kernel
       ↓
需要查詢 MAC
       ↓
發送 ARP Request
       ↓
送到 tap0
       ↓
我們的程式收到
```

所以才能看到 Ethernet Frame。

---

## Q3：如果 tap0 改成 10.0.0.10，還能收到嗎？

可以。

例如：

```bash
sudo ip addr add 10.0.0.10/24 dev tap0
```

此時：

```bash
ping 10.0.0.2
```

仍然會收到 ARP Request。

因為：

```text
10.0.0.10/24
```

與

```text
10.0.0.2
```

同樣位於：

```text
10.0.0.0/24
```

網段內。

Linux 仍然需要透過 ARP 查詢對方 MAC Address，因此會送出廣播封包。

---

# 今日總結

今天完成了整個網路堆疊的第一步：

```text
Linux Kernel
      ↓
   TAP Device
      ↓
   C Program
```

我們已經能夠：

* 建立 TAP 網卡
* 接收 Ethernet Frame
* 觀察真實封包內容
* 理解 Ethernet Header 的基本結構

這代表我們正式取得了網路世界最底層的資料流控制權。

---

# 下一堂課預告（Day 02）

明天我們將實作：

```text
Ethernet Frame Parser
```

把目前看到的 Raw Bytes：

```text
ff ff ff ff ff ff
52 54 00 12 34 56
08 06
...
```

解析成真正可閱讀的資料結構：

```text
Destination MAC
Source MAC
EtherType
Payload
```

正式踏入 Ethernet Protocol 的世界。
