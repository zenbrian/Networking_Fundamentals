#include "controller.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

static const char *get_mime_type(const char *path) {
    const char *dot = strrchr(path, '.'); // 找到最後一個點 '.'
    if (!dot) {
        return "text/plain";
    }
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css") == 0) return "text/css";
    if (strcmp(dot, ".js") == 0) return "application/javascript";
    if (strcmp(dot, ".png") == 0) return "image/png";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".ico") == 0) return "image/x-icon";
    if (strcmp(dot, ".svg") == 0) return "image/svg+xml";
    return "application/octet-stream";
}

int handle_root(const struct http_request *req, char *resp, size_t resp_len) {
    (void)req;
    return http_build_response(resp, resp_len,
        200, "OK", "text/html", "<h1>Hello World</h1>\n");
}

int handle_time(const struct http_request *req, char *resp, size_t resp_len) {
    (void)req;
    time_t now = time(NULL);
    char json[256];
    snprintf(json, sizeof(json),
        "{\"message\":\"Hello\",\"timestamp\":%ld}\n",
        (long)now);
    return http_build_response(resp, resp_len,
        200, "OK", "application/json", json);
}

int handle_info(const struct http_request *req, char *resp, size_t resp_len) {
    (void)req;
    const char *json = "{\"name\":\"My TCP Stack\",\"version\":\"1.0\"}\n";
    return http_build_response(resp, resp_len,
        200, "OK", "application/json", json);
}

int handle_not_found(const struct http_request *req, char *resp, size_t resp_len) {
    (void)req;
    return http_build_response(resp, resp_len,
        404, "Not Found", "text/html", "<h1>404 Not Found</h1>\n");
}

int handle_static_file(const struct http_request *req, char *resp, size_t resp_len) {
    // 1. 路徑對應 (Path Mapping)
    char filepath[512];
    if (strcmp(req->path, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "www/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "www%s", req->path);
    }

    // 2. 開啟磁碟檔案，若不存在則回傳 404
    FILE *fp = fopen(filepath, "rb");
    if (!fp) {
        return handle_not_found(req, resp, resp_len);
    }

    // 3. 取得檔案大小並讀入記憶體
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
    fclose(fp); // 關閉檔案防止 leak

    // 4. 自動判斷 MIME Type，使用二進位安全方式組裝 Response
    const char *content_type = get_mime_type(filepath);
    int total_len = http_build_binary_response(resp, resp_len, 200, "OK", content_type, body, (size_t)size);

    free(body); // 釋放記憶體防止 leak
    return total_len;
}

int http_route_dispatch(const struct http_request *req, char *resp, size_t resp_len) {
    if (strcmp(req->path, "/api/time") == 0) {
        return handle_time(req, resp, resp_len);
    } else if (strcmp(req->path, "/api/info") == 0) {
        return handle_info(req, resp, resp_len);
    } else {
        // 所有非 API 的路徑，全部交由靜態檔案處理器從 www/ 尋找
        return handle_static_file(req, resp, resp_len);
    }
}
