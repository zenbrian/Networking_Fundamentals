CC ?= gcc
CFLAGS ?= -Wall -Wextra -Iinclude

CORE_SRCS = src/ethernet.c src/arp.c src/arp_table.c src/ipv4.c src/icmp.c src/checksum.c src/routing.c src/udp.c src/tcp.c src/socket.c src/http.c src/controller.c

TARGETS = network send_arp send_arp_reply send_ipv4 send_icmp test_routing dns_client send_tcp_syn send_tcp_handshake send_tcp_data send_tcp_reassembly send_tcp_retransmit send_tcp_fin send_tcp_active_close
.PHONY: all clean

all: $(TARGETS)

network: src/tap.c $(CORE_SRCS)
	$(CC) $(CFLAGS) $^ -o $@

send_arp: test/send_arp.c src/arp.c src/arp_table.c
	$(CC) $(CFLAGS) $^ -o $@

send_arp_reply: test/send_arp_reply.c
	$(CC) $(CFLAGS) $^ -o $@

send_ipv4: test/send_ipv4.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_icmp: test/send_icmp.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

test_routing: test/test_routing.c src/routing.c
	$(CC) $(CFLAGS) $^ -o $@

dns_client: test/dns_client.c src/dns.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_syn: test/send_tcp_syn.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_handshake: test/send_tcp_handshake.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_data: test/send_tcp_data.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_reassembly: test/send_tcp_reassembly.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_retransmit: test/send_tcp_retransmit.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_fin: test/send_tcp_fin.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

send_tcp_active_close: test/send_tcp_active_close.c src/checksum.c
	$(CC) $(CFLAGS) $^ -o $@

clean:
	rm -f $(TARGETS) *.o


