CC     := gcc
STD    := -std=c11 -D_POSIX_C_SOURCE=200809L
WARN   := -Wall -Wextra -Werror
CFLAGS ?= $(STD) $(WARN) -O2

all: tsim

tsim: main.c
	$(CC) $(CFLAGS) -o $@ main.c

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(STD) $(WARN) -O0 -g -fsanitize=address,undefined"

clean:
	rm -f tsim

.PHONY: all debug clean
