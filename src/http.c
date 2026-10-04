#include "http.h"
#include <stdio.h>
#include <string.h>

int http_parse_request(char *buffer, struct http_request *req) {
    memset(req, 0, sizeof(*req));
    char *saveptr;
    char *line = strtok_r(buffer, "\r\n", &saveptr);
    if (!line) {
        return -1; // 連第一行都沒有，表示格式錯誤
    }
    if (sscanf(line, "%15s %255s %15s", req->method, req->path, req->version) != 3) {
        return -1; // 必須要有 3 個元素 (Method, Path, Version)
    }
    // 尋找 Host: 標頭
    while ((line = strtok_r(NULL, "\r\n", &saveptr)) != NULL) {
        if (sscanf(line, "Host: %255s", req->host) == 1) {
            break;
        }
    }
    return 0; // 解析成功
}


int http_build_binary_response(
    char *buffer,
    size_t buffer_size,
    int status_code,
    const char *status_text,
    const char *content_type,
    const void *body,
    size_t body_len
) {
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

    if (body && body_len > 0) {
        if ((size_t)header_len + body_len > buffer_size) {
            return -1; // 防止緩衝區溢位
        }
        memcpy(buffer + header_len, body, body_len);
    }

    return header_len + body_len;
}

int http_build_response(
    char *buffer,
    size_t buffer_size,
    int status_code,
    const char *status_text,
    const char *content_type,
    const char *body
) {
    if (!body) body = "";
    return http_build_binary_response(
        buffer, buffer_size,
        status_code, status_text, content_type,
        body, strlen(body)
    );
}


void http_print_request(const struct http_request *req) {
    printf("\n[HTTP Request]\n");
    printf("Method  = %s\n", req->method);
    printf("Path    = %s\n", req->path);
    printf("Version = %s\n", req->version);
    printf("Host    = %s\n\n", req->host);
}


