# Day 26：HTTP Request Parsing — 解析瀏覽器送來的 GET 請求

完成前面 25 天的底層開發後，我們正式從 TCP Socket API 往上走，進入應用層協定：**HTTP**。

```text
       Project Network Stack
+-----------------------------+
| Application Layer: HTTP     |  <--- ★ 今天在此！
+-----------------------------+
| Transport Layer: TCP        |
+-----------------------------+
| Network Layer: IPv4         |
+-----------------------------+
| Link Layer: Ethernet / ARP  |
+-----------------------------+
| Device: TAP Virtual NIC     |
+-----------------------------+
```

今天我們先把 Echo Server 往前推進一步：加入 **HTTP 請求解析器（HTTP Request Parser）**，讓伺服器能看懂瀏覽器與 `curl` 送來的 HTTP GET 請求。Day26 的重點是「解析 Request」；伺服器此時仍沿用 Echo 回傳，正式 HTTP Response 會在 Day27 補上。

---

# 今日學習目標與成果

- [x] 理解 HTTP Request-Line 與 Headers 是傳輸於 TCP 之上的文字格式。
- [x] 掌握 HTTP Request 結構（Request-Line、Headers 與 CRLF `\r\n` 邊界）。
- [x] 設計 `struct http_request` 結構體，作為應用層持有的 HTTP 資料容器。
- [x] 使用 `strtok_r` 與 `sscanf` 實作安全且防溢位的 Request-Line 欄位解析（Method, Path, Version）。
- [x] 實作 Header 逐行掃描迴圈，解析出關鍵標頭 `Host:`。
- [x] 封裝公開 API：`http_parse_request()` 與 `http_print_request()`。
- [x] 將 Parser 整合進 `src/tap.c` 的伺服器主迴圈。
- [x] 使用 `curl` 與 `nc` 實測，並深入理解 `Received HTTP/0.9 when not allowed` 的底層成因。

---

# 核心概念深入剖析

### 1. HTTP 到底是什麼？

很多人直覺以為瀏覽器與 Web 伺服器之間存在某種複雜的二進位協定或黑魔法。
HTTP/1.1 的 Request-Line 與 Headers 是人類可讀的文字格式，承載在 TCP 連線的資料串流（Byte Stream）之中；而 Body 則可以是文字，也可以是任意位元組資料。

當你在瀏覽器輸入網址，或用命令列發送請求時，網卡傳遞的實際 Payload 長得像這樣：

實際封包中，每一行以 `\r\n` 結尾，最後再用額外的一組 `\r\n` 表示 Headers 結束。

### 2. HTTP Request 報文的三段式結構

每一份合法的 HTTP 請求都嚴格遵循以下格式：

![Day26 HTTP Request 報文結構圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day26/Day26_1.png)


1. **第一行（Request-Line）**：
   - `Method`：執行的動作（例如 `GET`, `POST`, `PUT`, `DELETE`）。
   - `Path`：目標資源路徑（例如 `/`, `/hello`, `/api/user`）。
   - `Version`：協議版本（例如 `HTTP/1.1`）。
2. **標頭列表（Headers）**：
   - 鍵值對格式 `Header-Name: Value`。
   - 例如 `Host: 10.0.0.2:8080` 指名目標虛擬主機，這是 HTTP/1.1 的強制必備欄位。
3. **空行（Blank Line）**：
   - 由一對額外的 `\r\n` 組成，告知接收端「所有的標頭已經傳送完畢」。

---

### 3. C 語言字串解析利器：`strtok_r` 與 `sscanf`

在沒有內建 `split()` 或正則表達式的 C 語言中，如何兼顧效能與安全性？

![Day26 HTTP Parser 執行流程圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day26/Day26_2.png)

#### (1) `strtok_r(buffer, "\r\n", &saveptr)`
- **第一次呼叫**：傳入原始字串指標，它會把第一個遇到的換行符換成 `\0`，截斷出第一行（Request-Line），並將進度存於 `saveptr`。
- **後續呼叫**：傳入 `NULL`，表示「從 `saveptr` 記錄的斷點繼續往下讀取」，依序拿到每一行 Header，直到回傳 `NULL` 代表全部切完。

#### (2) `sscanf(line, "%15s %255s %15s", ...)`
- 類似輕量版的 pattern matching：自動依據空白字元拆出欄位。
- **緩衝區防溢位（Buffer Overflow Protection）**：加上寬度限制（如 `%15s`），保證最多只讀入 15 字元加上結尾 `\0`，避免惡意客戶端衝爆記憶體。
- **回傳值校驗**：`sscanf` 會回傳成功匹配的參數個數；若回傳值不等於 3，代表格式非法，可直接攔截報錯。

> **教學版限制**：本日 parser 使用 `strtok_r` 逐行切割，適合解析簡單 GET request；它會修改原始 buffer，且尚未處理跨多個 TCP segment 的 HTTP request、Header 名稱大小寫不敏感、Request Body、Chunked Transfer-Encoding 等完整 HTTP 行為。

---

# 核心資料結構與 API 實作

### 1. 結構定義與函式原型（`include/http.h`）

```c
#ifndef HTTP_H
#define HTTP_H

struct http_request {
    char method[16];   // 請求方法，如 "GET"
    char path[256];    // 請求路徑，如 "/hello"
    char version[16];  // 協定版本，如 "HTTP/1.1"
    char host[256];    // 目標主機，如 "10.0.0.2:8080"
};

// 解析 HTTP 請求，成功回傳 0，失敗回傳 -1
int http_parse_request(char *buffer, struct http_request *req);

// 除錯印出已解析的結構化請求
void http_print_request(const struct http_request *req);

#endif
```

---

### 2. HTTP 解析器實作（`src/http.c`）

```c
#include "http.h"
#include <stdio.h>
#include <string.h>

int http_parse_request(char *buffer, struct http_request *req) {
    // 1. 初始化結構體，清空舊殘留值
    memset(req, 0, sizeof(*req));

    // 2. 切出第一行 (Request Line)
    char *saveptr;
    char *line = strtok_r(buffer, "\r\n", &saveptr);
    if (!line) {
        return -1; // 缺乏第一行，格式錯誤
    }

    // 3. 解析 Method、Path、Version (安全讀取防溢位)
    if (sscanf(line, "%15s %255s %15s", req->method, req->path, req->version) != 3) {
        return -1; // 必須完整包含 3 個欄位
    }

    // 4. 逐行掃描 Headers，提取目標欄位 (如 Host)
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
        if (sscanf(line, "Host: %255s", req->host) == 1) {
            break; // 找到 Host 標頭後可提早結束
        }
    }

    return 0; // 解析成功
}

void http_print_request(const struct http_request *req) {
    printf("\n[HTTP Request]\n");
    printf("Method  = %s\n", req->method);
    printf("Path    = %s\n", req->path);
    printf("Version = %s\n", req->version);
    printf("Host    = %s\n\n", req->host);
}
```

---

### 3. 整合至伺服器主迴圈（`src/tap.c`）

在 `main()` 函式中，當 `socket_recv()` 取得 TCP Payload 後，傳入 `http_parse_request()` 進行解構：

```c
#include "http.h"
...

while (1) {
    struct socket *conn = socket_accept(listener);
    if (!conn) continue;

    uint8_t buf[1024];
    int n = socket_recv(conn, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        printf("\n[Echo Server] Received: %s\n", buf);

        // 呼叫 HTTP Parser
        struct http_request req;
        if (http_parse_request((char *)buf, &req) == 0) {
            http_print_request(&req);
        }

        socket_send(conn, buf, n);
    }

    socket_close(conn);
}
```

---

# 測試與驗證

### 步驟 1：編譯協定棧
```bash
make clean && make network
```

### 步驟 2：啟動伺服器與設定 TAP 網卡
**終端機 A（Server）**：
```bash
sudo ./network
```

**終端機 B（Client）**：
```bash
sudo ip addr add 10.0.0.1/24 dev tap0 2>/dev/null || true
sudo ip link set dev tap0 up
```

---

### 步驟 3：發送 GET 請求測試

```bash
curl http://10.0.0.2:8080/hello
```

#### 觀察 Server 端輸出：
```text
[Socket 1] Accepted connection from port 47794
[TCP DATA]
GET /hello HTTP/1.1
Host: 10.0.0.2:8080
User-Agent: curl/8.5.0
Accept: */*

[Echo Server] Received: GET /hello HTTP/1.1
Host: 10.0.0.2:8080
User-Agent: curl/8.5.0
Accept: */*

[HTTP Request]
Method  = GET
Path    = /hello
Version = HTTP/1.1
Host    = 10.0.0.2:8080

[TCP Active Close] ★ Initiating Active Close: Sending FIN...
[TCP Active Close] State -> FIN_WAIT_1 (Waiting for peer ACK)
...
[TCP TIME_WAIT] ★ 2-second Timer Expired -> State: CLOSED (Socket [01] released)
```

成功提取並印出：
- `Method = GET`
- `Path = /hello`
- `Version = HTTP/1.1`
- `Host = 10.0.0.2:8080`

---

# 深度問答：`curl: (1) Received HTTP/0.9 when not allowed`

在執行 `curl http://10.0.0.2:8080/hello` 時，Client 端報錯：
```text
curl: (1) Received HTTP/0.9 when not allowed
```

### 為什麼會這樣？
- 因為我們的伺服器此時仍然保留了舊的 Echo 行為：`socket_send(conn, buf, n)`，將收到整包字串原樣送回。
- 現代 `curl` 期待收到的合法 HTTP 回應**第一行必須是狀態行（Status-Line）**，如 `HTTP/1.1 200 OK`。
- 當伺服器回傳的是 `GET /hello ...` 開頭的字串時，`curl` 認為伺服器不支援 HTTP 狀態行，回退成了 1991 年最原始、僅支援純文字回傳的 **HTTP/0.9** 規格。基於安全考量，現代 `curl` 預設禁止 HTTP/0.9。

### 驗證驗收：
若加上 `--http0.9 --output -` 允許該模式並強制輸出至終端機：
```bash
curl --http0.9 --output - http://10.0.0.2:8080/hello
```
輸出回傳內容：
```text
GET /hello HTTP/1.1
Host: 10.0.0.2:8080
User-Agent: curl/8.5.0
Accept: */*
```
這證實了傳輸層雙向資料流通順暢，唯一缺少的就是應用層的 **HTTP 格式化回應**！

---

# 今日總結

今天我們把自製協定棧往應用層推進了一步：

1. **從位元組傳輸到結構解析**：以往伺服器只把 Payload 視為二進位串流，今天透過 `http_parse_request()`，可以解析出 Method、Path、Version 與 Host。
2. **為路由分流奠基**：有了 `req.path`，未來的伺服器就能針對 `/`、`/about`、`/api` 進行不同的業務邏輯分流。
3. **串起底層到應用層的教學主線**：
   `TAP` $\rightarrow$ `Ethernet` $\rightarrow$ `IPv4` $\rightarrow$ `TCP` $\rightarrow$ `Socket API` $\rightarrow$ `HTTP Parser`。

---

# Day 27 預告

今天我們已經能看懂瀏覽器送來的 `GET / HTTP/1.1` 了。
但瀏覽器此時還在苦苦等待伺服器的正式答覆！

明天（**Day 27**），我們將親手組裝並發送正式的 HTTP Response：
```http
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 21\r\n
\r\n
<h1>Hello World</h1>
```
讓真實的 Chrome 瀏覽器打開 `http://10.0.0.2:8080`，親眼看到我們手刻協定棧渲染出來的第一張網頁！

