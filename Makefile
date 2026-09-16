CC     := gcc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2

tsim: main.c
	$(CC) $(CFLAGS) -o $@ main.c

clean:
	rm -f tsim

.PHONY: clean
