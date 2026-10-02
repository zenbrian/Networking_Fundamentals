#include <stddef.h>

#ifndef HTTP_H
#define HTTP_H

struct http_request {
    char method[16];
    char path[256];
    char version[16];
    char host[256];
};

struct http_response {
    int status_code;
    const char *status_text;
    const char *content_type;
    const char *body;
};

int http_parse_request(char *buffer, struct http_request *req);

void http_print_request(const struct http_request *req);

int http_build_response(
    char *buffer,
    size_t buffer_size,
    int status_code,
    const char *status_text,
    const char *content_type,
    const char *body
);
#endif