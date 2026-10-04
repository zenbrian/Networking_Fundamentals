#ifndef CONTROLLER_H
#define CONTROLLER_H

#include <stddef.h>
#include "http.h"

/* 具體業務 Controller Handlers (回傳組裝的總 byte 數) */
int handle_root(const struct http_request *req, char *resp, size_t resp_len);
int handle_time(const struct http_request *req, char *resp, size_t resp_len);
int handle_info(const struct http_request *req, char *resp, size_t resp_len);
int handle_not_found(const struct http_request *req, char *resp, size_t resp_len);
int handle_static_file(const struct http_request *req, char *resp, size_t resp_len);

/* Router 總派送入口：根據 req->path 分流到對應 Handler */
int http_route_dispatch(const struct http_request *req, char *resp, size_t resp_len);

#endif
