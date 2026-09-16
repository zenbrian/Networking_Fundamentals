#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>
#include <unistd.h>

#include "ethernet.h"
#include "ipv4.h"
#include "tcp.h"
#include "config.h" // 取得 LOCAL_MAC 和 LOCAL_IP
#include "checksum.h"
#include "arp.h"    // 取得 arp_get_mac
#include "arp_table.h"

#define MAX_TCP_SOCKETS 64

static struct tcp_socket tcp_table[MAX_TCP_SOCKETS];

void tcp_init(void)
{
    for (int i = 0; i < MAX_TCP_SOCKETS; i++) {
        tcp_table[i].state = TCP_CLOSED;
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
        struct tcp_socket *conn = tcp_create_connection(ip->src_ip, ip->dst_ip, src_port, dst_port);
        if (conn == NULL) {
            printf("[TCP] Connection table full!\n");
            return;
        }
        uint32_t client_seq = ntohl(tcp->seq); // 取出對方的 SEQ (轉成 host byte order)
        conn->ack = client_seq + 1;            // 我們期待對方的下一號 (1001)
        conn->seq = 5000;                      // Server 自己的初始序號 ISN (先固定 5000)

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
            printf("\n========================================\n");
            printf("[TCP] ACK Received! Handshake Complete!\n");
            printf("[TCP] Connection Established: State -> ESTABLISHED\n");
            printf("========================================\n\n");
            tcp_dump_table();
            return;
        }
        if (conn->state == TCP_ESTABLISHED) {
            if (payload_len > 0) {
                printf("\n[TCP] Received Payload, Length = %zu bytes\n", payload_len);
                tcp_dump_payload(payload, payload_len);
                // 更新 ACK 號碼
                uint32_t received_seq = ntohl(tcp->seq);
                conn->ack = received_seq + payload_len;
                // 回傳 ACK 封包
                tcp_send_ack(fd, conn);
            }
            return;
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

