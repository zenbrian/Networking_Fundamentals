CC ?= gcc
CFLAGS ?= -Wall -Wextra -Iinclude

CORE_SRCS = src/ethernet.c src/arp.c src/arp_table.c src/ipv4.c src/checksum.c
TARGETS = network send_arp send_arp_reply send_ipv4

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

clean:
	rm -f $(TARGETS) *.o
