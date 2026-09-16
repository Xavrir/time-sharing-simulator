CC     := gcc
WARN   := -Wall -Wextra -Werror
STD    := -std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS ?= $(STD) $(WARN) -O2

SRC := src/main.c src/scheduler.c src/worker.c src/report.c
HDR := src/tsim.h
BIN := tsim

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC)

debug:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(STD) $(WARN) -O0 -g -fsanitize=address,undefined"

clean:
	rm -f $(BIN)

.PHONY: all debug clean
