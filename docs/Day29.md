# Day 29：Static File Server — 從磁碟讀取網頁檔案

在 Day 28 中，我們成功打造了 Dynamic JSON API，並將業務邏輯抽離成獨立的 Controller。但當時所有的回傳內容（如 `<h1>Hello World</h1>` 或 JSON 樣板）都是硬編碼（Hardcoded）在 C 語言程式碼中。

真正的 Web 伺服器（如 Nginx、Apache）的核心任務，是**直接從主機磁碟（Disk）讀取真實的網頁檔案（HTML、CSS、JavaScript、圖片與靜態資源），再經由網路協定堆疊封裝回傳給瀏覽器**。

```text
Browser / curl
    ↓ GET /index.html 或 GET /logo.png
TCP Stack (Handshake + Socket API)
    ↓
HTTP Parser (req.path)
    ↓
Path Mapping (www/index.html 或 www/logo.png)
    ↓
Disk I/O (fopen -> fseek/ftell -> fread -> fclose)
    ↓
MIME Type Detection (get_mime_type)
    ↓
Binary-Safe HTTP Response Builder (http_build_binary_response)
    ↓
socket_send() (二進位安全發送)
    ↓
瀏覽器成功載入網頁與圖片！
```

今天（**Day 29**），我們將伺服器升級為具備真實檔案讀取能力的 **Static File Server**，並突破二進位檔案（Null-Byte 截斷）的傳統實作陷阱！

---

# 今日學習目標與成果

- [x] 建立靜態網站根目錄 `www/` 並撰寫 `index.html` 與 `about.html`。
- [x] 實作路徑對應（Path Mapping）：將 `/` 自動對應至 `www/index.html`，一般路徑加上 `www` 前綴。
- [x] 實作 Linux 檔案 I/O 操作：使用 `fopen("rb")`、`fseek()`、`ftell()`、`fread()` 與 `fclose()`。
- [x] 資源洩漏防護：妥善處理 File Descriptor（`fclose`）與記憶體（`free`）。
- [x] 404 Not Found 防呆處理：當磁碟檔案不存在時回傳標準 404 頁面。
- [x] 實作 MIME Type 自動判斷器：依副檔名自動設定 `text/html`、`image/png`、`text/css`、`application/javascript` 等。
- [x] **深度突破：二進位檔案（Binary-Safe）傳輸**：
  - 剖析圖片二進位中的 `0x00`（Null Byte）如何導致 `strlen()` 與 `%s` 提早截斷。
  - 實作 `http_build_binary_response()`，使用 `memcpy` 與明確 `body_len` 解決二進位傳輸問題。
  - 擴充 `tap.c` 回應緩衝區至 64KB，並依精確 byte 數呼叫 `socket_send()`。
- [x] 支援圖片載入：在網頁中嵌入 `<img src="/logo.png">`，成功在瀏覽器與 curl 驗證！

---

# 核心概念深入剖析

### 1. 靜態檔案伺服器與磁碟 I/O 流程

當客戶端發起靜態資源請求時，伺服器經歷的路徑轉換與讀取流程如下：

![Day29 Static File Server 架構與磁碟 I/O 流程圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day29/Day29_1.png)

#### 磁碟讀取的四個關鍵階段：
1. **路徑映射（Path Mapping）**：
   - 請求 `/` ➔ 映射至 `www/index.html`（預設首頁）。
   - 請求 `/about.html` ➔ 映射至 `www/about.html`。
2. **檔案存在性檢查**：
   - 以二進位讀取模式開啟：`FILE *fp = fopen(filepath, "rb");`。
   - 若 `fp == NULL`，代表檔案不存在或無讀取權限，直接轉發給 `handle_not_found()`。
3. **動態測量檔案長度**：
   - `fseek(fp, 0, SEEK_END);` 移動到檔案尾端。
   - `long size = ftell(fp);` 取得精準位元組長度。
   - `rewind(fp);` 回到檔案開頭準備讀取。
4. **記憶體讀取與安全關閉**：
   - `char *body = malloc(size);` 配置緩衝區。
   - `fread(body, 1, size, fp);` 讀入資料。
   - **務必呼叫 `fclose(fp);`**，否則高併發下會迅速耗盡系統 File Descriptor！

> **實務安全補充**：本日的 path mapping 是教學版實作，目標是說明磁碟 I/O 與二進位回應。真實 Web Server 還必須阻擋 `..`、URL encoding 後的繞路、symbolic link 逃逸與超大檔案讀取，避免客戶端用 `/../../etc/passwd` 這類路徑讀到網站根目錄外的檔案。

---

### 2. 二進位檔案（圖片）與純文字檔案的本質差異

許多初學者實作 Web Server 時，HTML 都能正常顯示，但只要載入 `.png` 或 `.jpg` 就會嚴重破圖，原因就在於 **Null Byte (`0x00`)**！

![Day29 純文字與二進位檔案 Null Byte 安全性對比圖](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day29/Day29_2.png)

#### 踩雷點剖析：
1. **C 字串以 `\0` 作為結束符**：
   - HTML 或 JSON 是純 ASCII / UTF-8 文字，整段內容只在最後結尾才有 `\0`。
   - 但 PNG 檔案是二進位格式，開頭簽章（PNG Signature）第八個 byte 就是 `0x00`：
     ```text
     89 50 4E 47 0D 0A 1A 00 ...
     ```
2. **`strlen()` 會過早結束**：
   - 如果用 `strlen(body)` 計算圖片長度，結果只會回傳 7 bytes，導致 `Content-Length: 7`。
3. **`snprintf("%s")` 會截斷資料**：
   - `%s` 複製到第一個 `0x00` 就會停下來，整張圖片後面 99.9% 的資料全部遺失！
4. **`socket_send()` 必須使用明確長度**：
   - 發送時若用 `socket_send(conn, resp, strlen(resp))`，同樣會因為 Header 後的二進位內容含 `0x00` 而提早結束。

#### 徹底解法：二進位安全（Binary-Safe）函式
* 實作 `http_build_binary_response()`：傳入明確的 `size_t body_len`，並使用 `memcpy()` 拷貝資料。
* `http_route_dispatch()` 回傳精準組裝的 byte 長度，讓 `socket_send()` 完整送出每一顆 byte！

---

### 3. MIME Type（媒體類型）解析機制

瀏覽器完全仰賴 `Content-Type` 來決定用什麼引擎渲染收到的一串 byte：

| 副檔名 | Content-Type | 瀏覽器行為 |
| :--- | :--- | :--- |
| `.html` / `.htm` | `text/html` | 解析 HTML DOM 並排版為網頁畫面 |
| `.css` | `text/css` | 作為 CSS 樣式表解析並美化網頁 |
| `.js` | `application/javascript` | 交由 V8 / SpiderMonkey 引擎執行 JS 腳本 |
| `.png` | `image/png` | 啟動圖片解碼器，渲染點陣圖形 |
| `.jpg` / `.jpeg` | `image/jpeg` | 啟動 JPEG 解碼器，渲染照片圖形 |
| `.svg` | `image/svg+xml` | 渲染向量幾何圖形 |
| `.ico` | `image/x-icon` | 渲染網址列網站圖示（Favicon） |
| 未知副檔名 | `application/octet-stream` | 視為未知的二進位串流，通常觸發檔案下載 |

---

# 核心程式碼實作

### 1. 二進位安全回應組裝器（`src/http.c`）

```c
int http_build_binary_response(
    char *buffer,
    size_t buffer_size,
    int status_code,
    const char *status_text,
    const char *content_type,
    const void *body,
    size_t body_len
) {
    // 1. 組裝文字 Header
    int header_len = snprintf(buffer, buffer_size,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        status_code, status_text, content_type, body_len
    );

    if (header_len < 0 || (size_t)header_len >= buffer_size) {
        return -1;
    }

    // 2. 以 memcpy 安全拷貝二進位資料（不懼怕中間的 0x00）
    if (body && body_len > 0) {
        if ((size_t)header_len + body_len > buffer_size) {
            return -1; // 防止緩衝區溢位
        }
        memcpy(buffer + header_len, body, body_len);
    }

    return header_len + body_len;
}
```

### 2. 靜態檔案處理器與 MIME 判斷（`src/controller.c`）

```c
static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "text/plain";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

int handle_static_file(const struct http_request *req, char *resp, size_t resp_len) {
    char filepath[512];
    if (strcmp(req->path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "www/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "www%s", req->path);
    }

    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return handle_not_found(req, resp, resp_len);
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    rewind(fp);

    if (size < 0) {
        fclose(fp);
        return handle_not_found(req, resp, resp_len);
    }

    char *body = malloc(size);
    if (!body && size > 0) {
        fclose(fp);
        return handle_not_found(req, resp, resp_len);
    }

    if (size > 0) {
        fread(body, 1, size, fp);
    }
    fclose(fp); // 關閉檔案

    const char *content_type = get_mime_type(filepath);
    int total_len = http_build_binary_response(resp, resp_len, 200, "OK", content_type, body, (size_t)size);

    free(body); // 釋放記憶體
    return total_len;
}
```

> **可再加強處**：更嚴謹的版本會檢查 `fseek()`、`ftell()` 與 `fread()` 的回傳值，確認實際讀到的 byte 數等於檔案大小；若讀取不完整，應回傳 `500 Internal Server Error` 或關閉連線，而不是送出半截內容。

### 3. 主迴圈二進位發送升級（`src/tap.c`）

```c
char resp[65536]; // 升級為 64KB 緩衝區支援中大型網頁與圖檔

// 由 Controller 模組統一進行路由分流派送，取得精準位元組長度
int resp_len = http_route_dispatch(&req, resp, sizeof(resp));

if (resp_len > 0) {
    socket_send(conn, (const uint8_t *)resp, (size_t)resp_len);
}
```

---

# 驗收測試

### 1. 編譯並啟動
```bash
make clean && make network
sudo ./network
```

### 2. 測試預設首頁（自動映射至 `www/index.html`）
```bash
curl -i http://10.0.0.2:8080/
```
#### 輸出：
```http
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 167

<!DOCTYPE html>
<html>
<head>
    <title>My TCP Stack</title>
</head>
<body>
    <h1>Hello TCP/IP World</h1>
    <p><img src="/logo.png" alt="Logo"></p>
</body>
</html>
```

### 3. 測試讀取獨立頁面 `/about.html`
```bash
curl -i http://10.0.0.2:8080/about.html
```
#### 輸出：
```http
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 262
```

### 4. 測試二進位圖片下載 `/logo.png`
```bash
curl -i http://10.0.0.2:8080/logo.png
```
#### 輸出：
```http
HTTP/1.1 200 OK
Content-Type: image/png
Content-Length: 79

[79 bytes binary PNG data]
```

### 5. 測試不存在之檔案（404 Not Found）
```bash
curl -i http://10.0.0.2:8080/notfound.html
```
#### 輸出：
```http
HTTP/1.1 404 Not Found
Content-Type: text/html
Content-Length: 23

<h1>404 Not Found</h1>
```

### 6. 瀏覽器端真實渲染驗收（Windows 實測成功！）

透過轉發橋樑或路由設定，直接在 Windows 瀏覽器（如 Chrome / Edge）輸入 `http://localhost:8888/` 或 `http://10.0.0.2:8080/`：

![Day29 Windows 瀏覽器成功載入自製 TCP 堆疊託管之網頁與圖片](https://raw.githubusercontent.com/zenbrian/Networking_Fundamentals/refs/heads/main/docs/images/Day29/Day29_3.png)

- **HTML 標題**：成功解析並渲染大標題 `Hello TCP/IP World`。
- **二進位圖片**：成功發起第二個 HTTP 請求取得 `/logo.png`，並由自製 TCP Stack 完整送出二進位資料，圖片完美顯示（包含 iThome 標誌）！
- **HTTP 生命週期**：瀏覽器一鍵完成兩次 TCP 三向交握、HTTP GET、資料傳輸與連線關閉！

---

# 今日總結

1. **從記憶體寫死進化為真實檔案託管**：伺服器具備從硬碟動態載入 HTML、CSS、圖片等任意資產的能力。
2. **深入理解二進位與字串差異**：掌握了 `0x00` 在二進位協定與記憶體處理中的關鍵細節，成功避免常見的資料截斷 Bug。
3. **優雅的分層架構擴充**：昨天建立的 Controller 架構在此展現威力，新增靜態檔案功能時，完全不需改動底層 TCP 堆疊。

---

# Day 30 預告

到今天為止，我們已經從 Ethernet、ARP、IPv4、TCP 一路做到 HTTP Static File Server。明天（**Day 30**）不再新增單一協定，而是進行畢業總驗收：用 Wireshark 把 ARP、TCP 三向交握、HTTP GET、HTTP 200 OK 與 TCP 關閉流程完整串起來。

最終目標是回答那題經典問題：**「在瀏覽器輸入網址後，底層到底發生什麼事？」** 但這次不是背答案，而是用自己寫出來的協定棧、封包與時間序列親眼驗證。
