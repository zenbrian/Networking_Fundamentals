#ifndef TCP_H
#define TCP_H

#include <stdint.h>

/* IP Protocol 號碼 */
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif

/* TCP Flags 定義 */
#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

/* TCP Header 結構 (RFC 793 - 固定最小 20 Bytes) */
struct tcp_hdr
{
    uint16_t src_port;
    uint16_t dst_port;

    uint32_t seq;
    uint32_t ack;

    uint8_t data_offset;
    uint8_t flags; 

    uint16_t window;

    uint16_t checksum;

    uint16_t urgent_ptr;
} __attribute__((packed));

enum tcp_state
{
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED
};

/* TCP 亂序封包暫存結構 */
struct tcp_fragment
{
    uint32_t seq;
    uint16_t len;
    uint8_t data[1500];
    int used; // 0: 空位, 1: 已佔用
};

/* TCP 發送備份段落 (重傳緩衝區結構) */
struct tcp_segment
{
    uint32_t seq;       // 該封包發送時的起始 Sequence Number
    uint16_t len;       // Payload 資料長度
    uint8_t data[1500]; // Payload 資料內容備份
    uint64_t send_time; // 發送時間戳記 (毫秒 ms)
    int acked;          // 0: 未確認, 1: 已確認收到
    int used;           // 0: 空位, 1: 已佔用
    int retransmit_count; // 重傳次數
};



struct tcp_socket
{
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;              // 自己的 Send Sequence Number
    uint32_t ack;              // (保持紀錄相容)
    uint32_t expected_seq;     // ★ 我下一個期待收到的 Client Sequence Number
    enum tcp_state state;
    /* ★ 亂序重組 Buffer */
    struct tcp_fragment fragments[32];
    int fragment_count;
    /* ★ 重傳緩衝區 (發送端 Send Buffer & Retransmission Queue) */
    struct tcp_segment send_buffer[64];
    uint32_t last_ack;         // 記錄上次收到的 ACK 號碼
    int dup_ack_count;         // 重複 ACK 的累計次數
};

/* 印出 TCP 標頭資訊 */
void tcp_print_header(const struct tcp_hdr *tcp);
void tcp_receive(int fd, const uint8_t *buffer, size_t len);
void tcp_dump_payload(const uint8_t *data, size_t len);

void tcp_init(void);
int tcp_listen(uint16_t port);
struct tcp_socket* tcp_find_listener(uint16_t port);
struct tcp_socket* tcp_find_connection(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port);
struct tcp_socket* tcp_accept(void);

void tcp_dump_table(void);

int tcp_send_syn_ack(int fd, struct tcp_socket *conn);
int tcp_send_ack(int fd, struct tcp_socket *conn);
int tcp_send(int fd, struct tcp_socket *conn, const uint8_t *data, size_t len);
void tcp_send_data(int fd, struct tcp_socket *conn);
void tcp_check_retransmission(int fd);


#endif /* TCP_H */
