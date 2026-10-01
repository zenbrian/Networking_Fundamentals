#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "ethernet.h"
#include "ipv4.h"
#include "tcp.h"
#include "config.h" // 取得 LOCAL_MAC 和 LOCAL_IP
#include "checksum.h"
#include "arp.h"    // 取得 arp_get_mac
#include "arp_table.h"

static struct tcp_socket tcp_table[MAX_TCP_SOCKETS];

/* 輔助函式：取得當前系統時間 (毫秒 ms) */
static uint64_t get_current_time_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (uint64_t)tv.tv_usec / 1000;
}


void tcp_init(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        tcp_table[i].state = TCP_CLOSED;
        tcp_table[i].expected_seq = 0;
        tcp_table[i].fragment_count = 0;
        memset(tcp_table[i].fragments, 0, sizeof(tcp_table[i].fragments));

        tcp_table[i].last_ack = 0;
        tcp_table[i].dup_ack_count = 0;
        memset(tcp_table[i].send_buffer, 0, sizeof(tcp_table[i].send_buffer));
    }
}

int tcp_listen(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_LISTEN;
            tcp_table[i].dst_port = port;
            return 0;
        }
    }
    return -1;
}

struct tcp_socket* tcp_find_listener(uint16_t port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_LISTEN && tcp_table[i].dst_port == port) {
            return &tcp_table[i];
        }
    }
    return NULL;
}

struct tcp_socket* tcp_create_connection(uint32_t src_ip, uint32_t dst_ip,uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED) {
            tcp_table[i].state = TCP_SYN_RECEIVED;
            tcp_table[i].src_ip = src_ip;
            tcp_table[i].dst_ip = dst_ip;
            tcp_table[i].src_port = src_port;
            tcp_table[i].dst_port = dst_port;
            return &tcp_table[i];
        }
    }
    return NULL;
}

struct tcp_socket* tcp_find_connection(uint32_t src_ip, uint32_t dst_ip, uint16_t src_port, uint16_t dst_port)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state != TCP_CLOSED && tcp_table[i].state != TCP_LISTEN) {
            if (tcp_table[i].src_ip == src_ip &&
                tcp_table[i].dst_ip == dst_ip &&
                tcp_table[i].src_port == src_port &&
                tcp_table[i].dst_port == dst_port) {
                return &tcp_table[i];
            }
        }
    }
    return NULL;
}
struct tcp_socket* tcp_accept(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_ESTABLISHED) {
            return &tcp_table[i];
        }
    }
    return NULL;
}



void tcp_dump_table(void)
{
    printf("\n=== TCP SOCKET TABLE ===\n");
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_CLOSED)
            continue; // 空的不用印
        printf("[%02d] State: ", i);
        switch (tcp_table[i].state) {
            case TCP_LISTEN:       printf("LISTEN       "); break;
            case TCP_SYN_SENT:     printf("SYN_SENT     "); break;
            case TCP_SYN_RECEIVED: printf("SYN_RECEIVED "); break;
            case TCP_ESTABLISHED:  printf("ESTABLISHED  "); break;
            case TCP_FIN_WAIT_1:   printf("FIN_WAIT_1   "); break;
            case TCP_FIN_WAIT_2:   printf("FIN_WAIT_2   "); break;
            case TCP_CLOSE_WAIT:   printf("CLOSE_WAIT   "); break;
            case TCP_LAST_ACK:     printf("LAST_ACK     "); break;
            case TCP_TIME_WAIT:    printf("TIME_WAIT    "); break;
            default:               printf("UNKNOWN      "); break;
        }
        if (tcp_table[i].state == TCP_LISTEN) {
            printf("| Local Port: %u (*:* -> :%u)\n", tcp_table[i].dst_port, tcp_table[i].dst_port);
        } else {
            char sip_str[INET_ADDRSTRLEN];
            char dip_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &tcp_table[i].src_ip, sip_str, sizeof(sip_str));
            inet_ntop(AF_INET, &tcp_table[i].dst_ip, dip_str, sizeof(dip_str));
            printf("| %s:%u -> %s:%u\n",
                   sip_str, tcp_table[i].src_port,
                   dip_str, tcp_table[i].dst_port);
        }
    }
    printf("========================\n\n");
}



void tcp_print_header(const struct tcp_hdr *tcp)
{
    printf("\nTCP Packet\n");
    printf("----------------------------------------\n");

    printf("Source Port      : %u\n", ntohs(tcp->src_port));
    printf("Destination Port : %u\n", ntohs(tcp->dst_port));

    printf("SEQ              : %u\n", ntohl(tcp->seq));
    printf("ACK              : %u\n", ntohl(tcp->ack));

    printf("Data Offset      : %u (Header Length: %u bytes)\n",
           (tcp->data_offset >> 4),
           (tcp->data_offset >> 4) * 4);

    printf("Flags            : 0x%02x [ %s%s%s%s%s%s]\n",
           tcp->flags,
           (tcp->flags & TCP_SYN) ? "SYN " : "",
           (tcp->flags & TCP_ACK) ? "ACK " : "",
           (tcp->flags & TCP_FIN) ? "FIN " : "",
           (tcp->flags & TCP_RST) ? "RST " : "",
           (tcp->flags & TCP_PSH) ? "PSH " : "",
           (tcp->flags & TCP_URG) ? "URG " : "");

    printf("                   SYN = %d, ACK = %d, FIN = %d, RST = %d\n",
           (tcp->flags & TCP_SYN) ? 1 : 0,
           (tcp->flags & TCP_ACK) ? 1 : 0,
           (tcp->flags & TCP_FIN) ? 1 : 0,
           (tcp->flags & TCP_RST) ? 1 : 0);

    printf("Window           : %u\n", ntohs(tcp->window));
    printf("Checksum         : 0x%04x\n", ntohs(tcp->checksum));
    printf("Urgent Pointer   : %u\n", ntohs(tcp->urgent_ptr));
    printf("----------------------------------------\n");
}

void tcp_dump_payload(const uint8_t *data, size_t len)
{
    printf("\n[TCP DATA]\n");
    fwrite(data, 1, len, stdout);
    printf("\n");
    fflush(stdout);
}
/* 儲存亂序 (未來) 封包至 Buffer */
static void store_fragment(struct tcp_socket *conn, uint32_t seq, const uint8_t *data, uint16_t len)
{
    // 檢查是否重複存過
    for (int i = 0; i < 32; i++) {
        if (conn->fragments[i].used && conn->fragments[i].seq == seq) {
            printf("[TCP Buffer] Fragment SEQ=%u already buffered, ignore\n", seq);
            return;
        }
    }
    // 尋找空位存入
    for (int i = 0; i < 32; i++) {
        if (!conn->fragments[i].used) {
            conn->fragments[i].seq = seq;
            conn->fragments[i].len = len;
            memcpy(conn->fragments[i].data, data, len);
            conn->fragments[i].used = 1;
            conn->fragment_count++;
            printf("[TCP Buffer] Stored Out-of-Order Fragment: SEQ=%u, Len=%u (Total Buffered: %d)\n",
                   seq, len, conn->fragment_count);
            return;
        }
    }
    printf("[TCP Buffer] Fragment buffer full! Dropping SEQ=%u\n", seq);
}
/* 檢查並重組 Buffer 中的連續封包 */
static void process_buffered_fragments(struct tcp_socket *conn)
{
    int found = 1;
    while (found) {
        found = 0;
        for (int i = 0; i < 32; i++) {
            if (conn->fragments[i].used && conn->fragments[i].seq == conn->expected_seq) {
                printf("\n[TCP Reassembly] Found matching buffered fragment! SEQ=%u, Len=%u\n",
                       conn->fragments[i].seq, conn->fragments[i].len);
                
                // 交付給應用程式印出
                tcp_dump_payload(conn->fragments[i].data, conn->fragments[i].len);
                if (conn->recv_len + conn->fragments[i].len <= sizeof(conn->recv_buf)) {
                    memcpy(conn->recv_buf + conn->recv_len, conn->fragments[i].data, conn->fragments[i].len);
                    conn->recv_len += conn->fragments[i].len;
                }
                // 更新 expected_seq 與標記位子空出
                conn->expected_seq += conn->fragments[i].len;
                conn->fragments[i].used = 0;
                conn->fragment_count--;
                found = 1; // 繼續迴圈檢查是否有「連續」的下一個封包
                break;
            }
        }
    }
}

/* 重傳單一 Segment (保持原始 SEQ 不變) */
int tcp_resend_segment(int fd, struct tcp_socket *conn, struct tcp_segment *seg)
{
    size_t total_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + seg->len;
    uint8_t buffer[total_len];
    memset(buffer, 0, total_len);

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    uint8_t *payload = buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);

    memcpy(payload, seg->data, seg->len);

    // 封裝 TCP Header
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(seg->seq); // ★ 關鍵：保持原始序號，絕不推進！
    tcp->ack = htonl(conn->ack);
    tcp->data_offset = (sizeof(struct tcp_hdr) / 4) << 4;
    tcp->flags = TCP_ACK | TCP_PSH;
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(conn->dst_ip, conn->src_ip, tcp, sizeof(struct tcp_hdr) + seg->len);
    tcp->urgent_ptr = 0;

    // 封裝 IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + seg->len);
    ip->identification = htons(2004);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 封裝 Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP Retransmit] Send failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP Retransmit] write failed");
        return -1;
    }

    seg->send_time = get_current_time_ms(); // ★ 重設時間戳記
    printf("[TCP Retransmit] ★ Resent Segment: SEQ=%u, Len=%u\n", seg->seq, seg->len);
    return 0;
}

/* 處理收到的 ACK：釋放已確認緩衝區 / 偵測 Duplicate ACK / 觸發快速重傳 */
static void tcp_process_ack(int fd, struct tcp_socket *conn, uint32_t ack_num)
{
    if (ack_num > conn->last_ack) {
        // 情況 A：收到新 ACK
        printf("[TCP ACK] New ACK received: %u (Previous: %u)\n", ack_num, conn->last_ack);
        conn->last_ack = ack_num;
        conn->dup_ack_count = 0;

        // 釋放已確認收到的段落
        for (int i = 0; i < 64; i++) {
            if (conn->send_buffer[i].used) {
                if (conn->send_buffer[i].seq + conn->send_buffer[i].len <= ack_num) {
                    printf("[TCP Send Buffer] Segment SEQ=%u..%u acknowledged -> Free slot [%02d]\n",
                           conn->send_buffer[i].seq,
                           conn->send_buffer[i].seq + conn->send_buffer[i].len,
                           i);
                    conn->send_buffer[i].acked = 1;
                    conn->send_buffer[i].used = 0;
                }
            }
        }
    } else if (ack_num == conn->last_ack && conn->last_ack > 0) {
        // 情況 B：收到重複的 ACK
        conn->dup_ack_count++;
        printf("[TCP ACK] Duplicate ACK detected: %u (Count = %d)\n", ack_num, conn->dup_ack_count);

        // 快速重傳 (Fast Retransmit)
        if (conn->dup_ack_count == 3) {
            printf("\n[TCP Fast Retransmit] ★ 3 Duplicate ACKs received! Fast Retransmitting SEQ=%u...\n", ack_num);
            for (int i = 0; i < 64; i++) {
                if (conn->send_buffer[i].used && conn->send_buffer[i].seq == ack_num) {
                    tcp_resend_segment(fd, conn, &conn->send_buffer[i]);
                    break;
                }
            }
        }
    }
}

void tcp_receive(int fd, const uint8_t *buffer, size_t len)
{
    // 取得 IPv4 標頭以計算實際 IP Header 長度
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;

    // 檢查封包總長度是否足夠容納 TCP Header
    if (len < ETH_HEADER_LEN + ip_hdr_len + sizeof(struct tcp_hdr)) {
        printf("[TCP] Packet too short\n");
        return;
    }
    
    // 趁著剛收到封包，把對方的 IP 與 MAC 記錄到通訊錄！
    struct ethernet_hdr *rx_eth = (struct ethernet_hdr *)buffer;
    arp_table_insert((const uint8_t *)&ip->src_ip, rx_eth->src);

    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + ip_hdr_len);
    tcp_print_header(tcp);

    size_t tcp_hdr_len = (tcp->data_offset >> 4) * 4;
    const uint8_t *payload = (const uint8_t *)tcp + tcp_hdr_len;
    uint16_t ip_total_len = ntohs(ip->total_length);
    size_t payload_len = 0;
    if (ip_total_len >= ip_hdr_len + tcp_hdr_len) {
        payload_len = ip_total_len - ip_hdr_len - tcp_hdr_len;
    }

    if (tcp->flags & TCP_SYN) {
        uint16_t dst_port = ntohs(tcp->dst_port);
        uint16_t src_port = ntohs(tcp->src_port);

        struct tcp_socket *listener = tcp_find_listener(dst_port);
        if (listener == NULL) {
            printf("[TCP] Port %u not listening -> DROP\n", dst_port);
            return;
        }
        printf("[TCP] Incoming SYN on Port %u\n", dst_port);
        struct tcp_socket *conn = tcp_find_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            conn = tcp_create_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        }
        if (conn == NULL) {
            printf("[TCP] Connection table full!\n");
            return;
        }
        uint32_t client_seq = ntohl(tcp->seq); // 取出對方的 SEQ (轉成 host byte order)
        conn->ack = client_seq + 1;            // 我們期待對方的下一號 (1001)
        conn->expected_seq = client_seq + 1;   // 初始化期待的 Sequence Number
        conn->seq = 5000;                      // Server 自己的初始序號 ISN (先固定 5000)

        conn->last_ack = 0;
        conn->dup_ack_count = 0;
        memset(conn->send_buffer, 0, sizeof(conn->send_buffer));

        tcp_dump_table();

        tcp_send_syn_ack(fd, conn);


        return;
    }

    if ((tcp->flags & TCP_ACK) && !(tcp->flags & TCP_SYN)) {
        uint16_t src_port = ntohs(tcp->src_port);
        uint16_t dst_port = ntohs(tcp->dst_port);
        // 1. 透過四元組尋找是否已有這條連線
        struct tcp_socket *conn = tcp_find_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] No matching connection for ACK -> DROP\n");
            return;
        }
        // 2. 檢查狀態是否正在等待第三次交握 (SYN_RECEIVED)
        if (conn->state == TCP_SYN_RECEIVED) {
            uint32_t ack_num = ntohl(tcp->ack);
            uint32_t expected_ack = conn->seq + 1; // 5000 + 1 = 5001
            // 3. 驗證 ACK 號碼是否正確
            if (ack_num != expected_ack) {
                printf("[TCP] Invalid ACK number: %u (expected %u) -> DROP\n", ack_num, expected_ack);
                return;
            }
            // 4. 握手成功！狀態轉移到 ESTABLISHED
            conn->state = TCP_ESTABLISHED;
            conn->seq++; // 消耗掉 SYN 的序號，自己的 SEQ 正式推進到 5001
            conn->last_ack = ack_num;
            conn->dup_ack_count = 0;
            printf("\n========================================\n");
            printf("[TCP] ACK Received! Handshake Complete!\n");
            printf("[TCP] Connection Established: State -> ESTABLISHED\n");
            printf("========================================\n\n");
            tcp_dump_table();
            return;
        }
        if (conn->state == TCP_ESTABLISHED) {
            uint32_t ack_num = ntohl(tcp->ack);
            tcp_process_ack(fd, conn, ack_num);

             if (tcp->flags & TCP_FIN) {
                uint32_t received_seq = ntohl(tcp->seq);
                printf("\n========================================\n");
                printf("[TCP] FIN Received from Client! (SEQ=%u)\n", received_seq);
                // FIN 消耗 1 個序號 (如果有夾帶 payload 也要一起算進去)
                conn->ack = received_seq + payload_len + 1;
                // 第二次揮手：Server 回覆 ACK，進入 CLOSE_WAIT
                printf("[TCP] Sending ACK for FIN -> State: CLOSE_WAIT\n");
                conn->state = TCP_CLOSE_WAIT;
                tcp_send_ack(fd, conn);
                // 第三次揮手：Server 也發送自己的 FIN，進入 LAST_ACK
                printf("[TCP] Server closing -> Sending FIN -> State: LAST_ACK\n");
                conn->state = TCP_LAST_ACK;
                tcp_send_fin(fd, conn);
                printf("========================================\n\n");
                return;
            }
            if (payload_len > 0) {
                uint32_t received_seq = ntohl(tcp->seq);
                printf("\n[TCP] Received Payload: SEQ=%u, Len=%zu (Expected SEQ=%u)\n",
                       received_seq, payload_len, conn->expected_seq);

                if (received_seq == conn->expected_seq) {
                    // 情況一：按順序到達 (In-Order)
                    printf("[TCP Reassembly] Packet In-Order! Delivering to application...\n");
                    tcp_dump_payload(payload, payload_len);
                    if (conn->recv_len + payload_len <= sizeof(conn->recv_buf)) {
                        memcpy(conn->recv_buf + conn->recv_len, payload, payload_len);
                        conn->recv_len += payload_len;
                    }

                    // 1. 推進期待序號
                    conn->expected_seq += payload_len;

                    // 2. 檢查並處理暫存區裡的亂序封包 (連鎖反應)
                    process_buffered_fragments(conn);
                    
                    // ★ 收到正常依序的請求，回送回應資料

                    // ★ 若 Client 請求中包含 "CLOSE" 關鍵字，觸發 Server 主動關閉
                    // int trigger_close = 0;
                    // for (size_t k = 0; k + 5 <= payload_len; k++) {
                    //     if (memcmp(payload + k, "CLOSE", 5) == 0) {
                    //         trigger_close = 1;
                    //         break;
                    //     }
                    // }
                    // if (trigger_close) {
                    //     tcp_close(fd, conn);
                    // }

                } else if (received_seq > conn->expected_seq) {
                    // 情況二：亂序封包 (Out-of-Order / Future Packet)
                    printf("[TCP Reassembly] Out-of-Order Packet detected! (Missing bytes before SEQ %u)\n", received_seq);
                    store_fragment(conn, received_seq, payload, payload_len);

                } else {
                    // 情況三：重複封包 (Duplicate Packet)
                    printf("[TCP Reassembly] Duplicate Packet (SEQ=%u < Expected=%u) -> Ignore payload\n",
                           received_seq, conn->expected_seq);
                }

                // ★ 累積確認 (Cumulative ACK)：總是回覆目前連續接收到的 expected_seq
                conn->ack = conn->expected_seq;
                tcp_send_ack(fd, conn);
            }
            return;
        }
        // ★ 2. 第四次揮手：收到 Client 回傳的最終 ACK
        if (conn->state == TCP_LAST_ACK) {
            if (tcp->flags & TCP_ACK) {
                conn->state = TCP_CLOSED;
                printf("\n========================================\n");
                printf("[TCP] Final ACK Received!\n");
                printf("[TCP] Four-Way Teardown Complete: State -> CLOSED\n");
                printf("========================================\n\n");
                tcp_dump_table();
                return;
            }
            // ★ 主動關閉路徑 1：Server 處於 FIN_WAIT_1，等待對方確認我方 FIN 的 ACK
           
        }
        if (conn->state == TCP_FIN_WAIT_1) {
            if (tcp->flags & TCP_ACK) {
                uint32_t ack_num = ntohl(tcp->ack);
                if (ack_num == conn->seq) {
                    conn->state = TCP_FIN_WAIT_2;
                    printf("\n========================================\n");
                    printf("[TCP Active Close] ACK for our FIN received! (ACK=%u)\n", ack_num);
                    printf("[TCP Active Close] State -> FIN_WAIT_2 (Waiting for peer FIN)\n");
                    printf("========================================\n\n");
                    tcp_dump_table();
                    return;
                }
            }
        }
        // ★ 主動關閉路徑 2：Server 處於 FIN_WAIT_2，等待對方的 FIN
        if (conn->state == TCP_FIN_WAIT_2) {
            if (tcp->flags & TCP_FIN) {
                uint32_t received_seq = ntohl(tcp->seq);
                printf("\n========================================\n");
                printf("[TCP Active Close] Peer FIN received! (SEQ=%u)\n", received_seq);
                conn->ack = received_seq + payload_len + 1;
                printf("[TCP Active Close] Sending Final ACK -> State: TIME_WAIT\n");
                tcp_send_ack(fd, conn);
                conn->state = TCP_TIME_WAIT;
                conn->time_wait_start = get_current_time_ms();
                printf("[TCP Active Close] State -> TIME_WAIT (2-second timer started)\n");
                printf("========================================\n\n");
                tcp_dump_table();
                return;
            }
        }
        // ★ 主動關閉路徑 3：TIME_WAIT 留守期間若對方重送 FIN，再度補發 ACK
        if (conn->state == TCP_TIME_WAIT) {
            if (tcp->flags & TCP_FIN) {
                printf("[TCP TIME_WAIT] Retransmitted Peer FIN received -> Resending Final ACK\n");
                tcp_send_ack(fd, conn);
                return;
            }
        }

    }
}

    
int tcp_send_syn_ack(int fd, struct tcp_socket *conn){
    //1.準備Buffer與切割各層指標
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));
    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
    //2. 封裝 TCP Header
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq); // 5000
    tcp->ack = htonl(conn->ack); // 1001
    tcp->data_offset = (5 << 4); // 只有 Header
    tcp->window = htons(4096); // 客戶端通常會給很大
    tcp->flags = (TCP_ACK | TCP_SYN);
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(conn->dst_ip, conn->src_ip, tcp, sizeof(struct tcp_hdr));
    tcp->urgent_ptr = 0;
    //3. 封裝 Layer 3: IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)); // Header + TCP Header (假設沒有 Data)
    ip->identification = htons(2001);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP; // 6: TCP
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));
    //4. 封裝 Layer 2: Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send SYN-ACK failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);
    //5. 寫出到虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write SYN-ACK failed");
        return -1;
    }
    printf("[TCP] Sent SYN-ACK: SEQ=%u, ACK=%u\n", conn->seq, conn->ack);
    return 0;

}

int tcp_send_ack(int fd, struct tcp_socket *conn)
{
    // 1. 準備 Buffer 與切割各層指標
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 2. 封裝 TCP Header (純 ACK，無 Payload)
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq); // Client 回傳 ACK，SEQ 通常是上一次收到的 SEQ + 1
    tcp->ack = htonl(conn->ack); // 加上 Server SEQ
    tcp->data_offset = (5 << 4); // 只有 Header
    tcp->window = htons(4096);
    tcp->flags = TCP_ACK; // 純 ACK Flag
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(conn->dst_ip, conn->src_ip, tcp, sizeof(struct tcp_hdr));
    tcp->urgent_ptr = 0;

    // 3. 封裝 Layer 3: IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(2002); // 隨意編號
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP; // 6: TCP
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 4. 封裝 Layer 2: Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send ACK failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 5. 寫出到虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write ACK failed");
        return -1;
    }

    // 更新狀態？(Optional，視需求)
    // conn->state = TCP_ESTABLISHED;

    printf("[TCP] Sent ACK: SEQ=%u, ACK=%u\n", conn->seq, conn->ack);

    return 0;
}

int tcp_send_fin(int fd, struct tcp_socket *conn)
{
    // 1. 準備 Buffer 與指標 (無 Payload，總長度 54 Bytes)
    uint8_t buffer[ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr)];
    memset(buffer, 0, sizeof(buffer));

    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));

    // 2. 封裝 TCP Header
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq);
    tcp->ack = htonl(conn->ack);
    tcp->data_offset = (5 << 4);
    tcp->window = htons(4096);
    tcp->flags = TCP_FIN | TCP_ACK; // ★ 關鍵：帶上 FIN 與 ACK
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(conn->dst_ip, conn->src_ip, tcp, sizeof(struct tcp_hdr));
    tcp->urgent_ptr = 0;

    // 3. 封裝 IPv4 Header
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr));
    ip->identification = htons(2005);
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP;
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    // 4. 封裝 Ethernet Header (查詢 ARP Table)
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send FIN failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);

    // 5. 寫出到 TAP 虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write FIN failed");
        return -1;
    }

    //  關鍵：FIN 消耗 1 個序號！
    conn->seq++;
    printf("[TCP] Sent FIN-ACK: SEQ=%u, ACK=%u (seq advanced to %u)\n", 
           ntohl(tcp->seq), conn->ack, conn->seq);
    return 0;
}


int tcp_send(int fd, struct tcp_socket *conn, const uint8_t *data, size_t len){
    // 1. 計算總長度：L2 (14) + L3 (20) + L4 (20) + 應用層資料長度 (len)
    size_t total_len = ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + len;
    uint8_t buffer[total_len];
    memset(buffer, 0, total_len);
    
     // 2. 切割記憶體指標（把長紙帶切分成四段）
    struct ethernet_hdr *eth = (struct ethernet_hdr *)buffer;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(buffer + ETH_HEADER_LEN);
    struct tcp_hdr *tcp = (struct tcp_hdr *)(buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr));
     // 3. payload 指標緊接在 TCP Header 後面
    uint8_t *payload = buffer + ETH_HEADER_LEN + sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr);
    
    // 4. 搬移應用層資料到 Payload 區域
    memcpy(payload, data, len); 

    //封裝tcp
    tcp->src_port = htons(conn->dst_port);
    tcp->dst_port = htons(conn->src_port);
    tcp->seq = htonl(conn->seq); 
    tcp->ack = htonl(conn->ack); 
    tcp->data_offset = ((sizeof(struct tcp_hdr)) / 4 << 4);
    tcp->flags = TCP_ACK | TCP_PSH;        // ACK + PSH
    tcp->window = htons(4096);
    tcp->checksum = 0;
    tcp->checksum = tcp_checksum(conn->dst_ip, conn->src_ip, tcp, sizeof(struct tcp_hdr) + len);
    tcp->urgent_ptr = 0;

    //封裝IPv4
    ip->version_ihl = (4 << 4) | 5;
    ip->tos = 0;
    ip->total_length = htons(sizeof(struct ipv4_hdr) + sizeof(struct tcp_hdr) + len);
    ip->identification = htons(2003); // 隨意編號
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = IPPROTO_TCP; // 6: TCP
    ip->src_ip = conn->dst_ip;
    ip->dst_ip = conn->src_ip;
    ip->checksum = 0;
    ip->checksum = ipv4_checksum(ip, sizeof(struct ipv4_hdr));

    //封裝封裝 Ethernet Header
    struct arp_entry *entry = arp_table_lookup((const uint8_t *)&conn->src_ip);
    if (!entry || !entry->valid) {
        printf("[TCP] Send Data failed: MAC not in ARP table\n");
        return -1;
    }
    memcpy(eth->dst, entry->mac, ETH_ADDR_LEN);
    memcpy(eth->src, LOCAL_MAC, ETH_ADDR_LEN);
    eth->ethertype = htons(ETHERTYPE_IPV4);
    
    // 5. 寫出到虛擬網卡
    ssize_t sent = write(fd, buffer, sizeof(buffer));
    if (sent < 0) {
        perror("[TCP] write Data failed");
        return -1;
    }
    
    for (int i = 0; i < 64; i++) {
        if (!conn->send_buffer[i].used) {
            conn->send_buffer[i].seq = conn->seq;
            conn->send_buffer[i].len = len;
            memcpy(conn->send_buffer[i].data, data, len);
            conn->send_buffer[i].send_time = get_current_time_ms();
            conn->send_buffer[i].acked = 0;
            conn->send_buffer[i].used = 1;
            printf("[TCP Send Buffer] Saved segment in slot [%02d]: SEQ=%u, Len=%zu\n",
                   i, conn->seq, len);
            break;
        }
    }

    conn->seq += len;
    printf("[TCP] Sent Data: %zu bytes | New SEQ=%u, ACK=%u\n", len, conn->seq, conn->ack);
    return 0;

}

void tcp_send_data(int fd, struct tcp_socket *conn)
{
    const char *msg = "Hello from My TCP Stack\n";
    tcp_send(fd, conn, (const uint8_t *)msg, strlen(msg));
}

/* 超時重傳計時器檢查：掃描所有連線中超過 1000ms 尚未被 ACK 的 Segment */
/* 超時重傳計時器檢查 */
void tcp_check_retransmission(int fd)
{
    uint64_t now = get_current_time_ms();

    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_ESTABLISHED) {
            for (int j = 0; j < 64; j++) {
                if (tcp_table[i].send_buffer[j].used && !tcp_table[i].send_buffer[j].acked) {
                    if (now - tcp_table[i].send_buffer[j].send_time >= 1000) {
                        
                        // ★ 若重傳超過 5 次，判定對方離線，放棄該封包
                        if (tcp_table[i].send_buffer[j].retransmit_count >= 5) {
                            printf("\n[TCP Retransmit] Max retries (5) reached for SEQ=%u. Dropping.\n",
                                   tcp_table[i].send_buffer[j].seq);
                            tcp_table[i].send_buffer[j].used = 0;
                            continue;
                        }

                        tcp_table[i].send_buffer[j].retransmit_count++;
                        printf("\n[TCP Timeout Retransmit] ★ Segment SEQ=%u timeout (%llu ms, Retry #%d)! Retransmitting...\n",
                               tcp_table[i].send_buffer[j].seq,
                               (unsigned long long)(now - tcp_table[i].send_buffer[j].send_time),
                               tcp_table[i].send_buffer[j].retransmit_count);
                        tcp_resend_segment(fd, &tcp_table[i], &tcp_table[i].send_buffer[j]);
                    }
                }
            }
        }
    }
}


/* ★ Day 24 主動關閉入口：對 ESTABLISHED 連線發送 FIN 並轉為 FIN_WAIT_1 */
int tcp_close(int fd, struct tcp_socket *conn)
{
    if (!conn || conn->state != TCP_ESTABLISHED) {
        printf("[TCP Active Close] Error: Connection not ESTABLISHED (cannot close)\n");
        return -1;
    }

    printf("\n[TCP Active Close] ★ Initiating Active Close: Sending FIN...\n");
    if (tcp_send_fin(fd, conn) < 0) {
        return -1;
    }

    conn->state = TCP_FIN_WAIT_1;
    printf("[TCP Active Close] State -> FIN_WAIT_1 (Waiting for peer ACK)\n");
    tcp_dump_table();
    return 0;
}

#define TCP_TIME_WAIT_MS 2000
void tcp_check_time_wait(void)
{
    uint64_t now = get_current_time_ms();
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        if (tcp_table[i].state == TCP_TIME_WAIT) {
            if (now - tcp_table[i].time_wait_start >= TCP_TIME_WAIT_MS) {
                printf("\n[TCP TIME_WAIT] ★ 2-second Timer Expired -> State: CLOSED (Socket [%02d] released)\n", i);
                tcp_table[i].state = TCP_CLOSED;
                tcp_dump_table();
            }
        }
    }
}

struct tcp_socket* tcp_get_socket(int index){
    if(index < 0 || index >= MAX_TCP_SOCKETS)
        return NULL;
    return &tcp_table[index];
}




