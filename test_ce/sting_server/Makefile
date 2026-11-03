# host build of the test server for test_ce/test_sting (the STinG compatibility test)
CC     ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

sting_server: sting_server.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f sting_server

.PHONY: clean
