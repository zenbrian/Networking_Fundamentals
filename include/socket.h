#ifndef SOCKET_H
#define SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include "tcp.h"

enum socket_state
{
    SOCKET_UNUSED = 0,
    SOCKET_CLOSED,
    SOCKET_LISTEN,
    SOCKET_ESTABLISHED,
};


struct socket
{
    int id;                  // 號碼牌 (Handle，類似 fd)
    uint16_t port;           // 綁定的通訊埠
    int state;               // Socket 當前狀態
    struct tcp_socket *tcp;  // 指向底層真正的 TCP 連線結構
};

struct socket* socket_create(void);
int socket_bind(struct socket *sock, uint16_t port);
int socket_listen(struct socket *sock);
struct socket* socket_accept(struct socket *listener);
int socket_recv(struct socket *sock, uint8_t *buf, size_t len);
int socket_send(struct socket *sock, const uint8_t *buf, size_t len);
int socket_close(struct socket *sock);

#endif
