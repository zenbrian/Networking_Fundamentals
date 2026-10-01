#include "socket.h"
#include "tcp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

#define MAX_SOCKETS 64
static struct socket socket_table[MAX_SOCKETS];
static int socket_init_done = 0;

static void socket_init_once(void){
    if(!socket_init_done){
        for(int i = 0; i < MAX_SOCKETS; i++){
            socket_table[i].state = SOCKET_UNUSED;
        }
        socket_init_done = 1;
    }
}

struct socket* socket_create(void){
    socket_init_once();
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (socket_table[i].state == SOCKET_UNUSED) {
            socket_table[i].id = i;
            socket_table[i].port = 0;
            socket_table[i].state = SOCKET_CLOSED;
            socket_table[i].tcp = NULL;
            return &socket_table[i];
        }
    }
    return NULL;
}

int socket_bind(struct socket *sock, uint16_t port){
    if(sock == NULL || sock->state != SOCKET_CLOSED)
        return -1;
    sock->port = port;
    return 0;
}

int socket_listen(struct socket *sock){
    if(sock == NULL || sock->port == 0 ||sock->state != SOCKET_CLOSED)
        return -1;
    tcp_listen(sock->port);
    sock->tcp = tcp_find_listener(sock->port);
    sock->state = SOCKET_LISTEN;
    printf("[Socket %d] Listening on Port %u\n", sock->id, sock->port);
    return 0;
}

// 檢查某個底層 tcp 連線是否已經被上層某個 socket 認領
static int is_tcp_already_accepted(struct tcp_socket *tcp)
{
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (socket_table[i].state == SOCKET_ESTABLISHED && socket_table[i].tcp == tcp) {
            return 1; // 已經有人認領了
        }
    }
    return 0; // 還沒有人認領！
}

static struct tcp_socket* find_new_established(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        struct tcp_socket *t = tcp_get_socket(i);
        if (t && t->state == TCP_ESTABLISHED && t->dst_port == port) {
            if (!is_tcp_already_accepted(t)) {
                return t; // 找到剛建立、且還沒被認領的連線！
            }
        }
    }
    return NULL;
}

// 宣告外部的心跳函式
extern int net_poll(void);
extern int get_tap_fd(void);

struct socket* socket_accept(struct socket *listener)
{
    // 1. 檢查 listener 是否合法且處於 SOCKET_LISTEN 狀態
    if (listener == NULL || listener->state != SOCKET_LISTEN) {
        return NULL;
    }
    // 2. 迴圈等待新連線
    while (1) {
        // A. 找找看有沒有剛完成交握、尚未被認領的連線
        struct tcp_socket *t = find_new_established(listener->port);
        if (t != NULL) {
            // 找到了！建立一個新 socket 給應用程式
            struct socket *conn = socket_create();
            if (conn == NULL) return NULL;
            conn->state = SOCKET_ESTABLISHED;
            conn->port = t->dst_port;
            conn->tcp = t;
            printf("[Socket %d] Accepted connection from port %u\n", conn->id, t->src_port);
            return conn;
        }
        // B. 還沒有新連線，就驅動底層收發封包
        net_poll();
    }
}

int socket_recv(struct socket *sock, uint8_t *buf, size_t len){
    if(sock == NULL || sock->tcp == NULL || buf == NULL || len == 0)
        return -1;
    while (sock->tcp->recv_len == 0){
        if(sock->tcp->state == TCP_CLOSED || sock->tcp->state == TCP_CLOSE_WAIT){
            return 0;
        }
        net_poll();
    }
    // 1. 計算這次要複製多少 Bytes (不能超過應用程式提供的 len，也不能超過郵箱裡的 recv_len)
    size_t to_copy = (sock->tcp->recv_len < len) ? sock->tcp->recv_len : len;
    // 2. 將資料複製給應用程式的 buf
    memcpy(buf, sock->tcp->recv_buf, to_copy);
    // 3. 郵箱扣除已讀取的數量
    sock->tcp->recv_len -= to_copy;
    // 4. 如果郵箱裡還有剩下的資料，把它往前推到開頭
    if (sock->tcp->recv_len > 0) {
        memmove(sock->tcp->recv_buf, sock->tcp->recv_buf + to_copy, sock->tcp->recv_len);
    }
    // 5. 回傳實際讀取到的位元組數
    return (int)to_copy;
}

int socket_send(struct socket *sock, const uint8_t *buf, size_t len){
    if(sock == NULL || sock->tcp == NULL || buf == NULL || sock->tcp->state != TCP_ESTABLISHED)
        return -1;
    int fd = get_tap_fd();
    tcp_send(fd, sock->tcp, buf, len); 
    return len;  
}

int socket_close(struct socket *sock){
    if(sock == NULL)
        return -1;
    int fd = get_tap_fd();
    tcp_close(fd, sock->tcp);
    sock->state = SOCKET_CLOSED;  
    sock->tcp = NULL;
    return 0;
}