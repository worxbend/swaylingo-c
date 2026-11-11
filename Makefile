CC ?= cc
SANITIZE ?=

WARNINGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -Wpedantic \
	-Wshadow -fno-common -fstack-protector-strong

SANITIZE_FLAGS =
ifeq ($(SANITIZE),1)
SANITIZE_FLAGS = -fsanitize=address,undefined -fno-omit-frame-pointer
endif

override CPPFLAGS += -Iinclude
override CFLAGS += $(WARNINGS) $(SANITIZE_FLAGS)

LIB_SRCS = src/util.c src/config.c src/memory.c src/json.c src/sway_ipc.c
HEADERS = include/skbd/util.h include/skbd/config.h include/skbd/memory.h \
	include/skbd/json.h include/skbd/sway.h include/skbd/runtime.h

.PHONY: all test clean

all: bin/skbd build/test_memory build/test_config build/test_json \
	build/test_ipc build/test_cli build/test_runtime

bin:
	mkdir -p bin

build:
	mkdir -p build

bin/skbd: src/main.c $(LIB_SRCS) $(HEADERS) | bin
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ src/main.c $(LIB_SRCS)

build/test_memory: tests/test_memory.c src/memory.c src/config.c src/util.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_memory.c src/memory.c src/config.c src/util.c

build/test_config: tests/test_config.c src/config.c src/util.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_config.c src/config.c src/util.c

build/test_json: tests/test_json.c src/json.c src/util.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_json.c src/json.c src/util.c

build/test_ipc: tests/test_ipc.c src/sway_ipc.c src/json.c src/util.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ tests/test_ipc.c src/sway_ipc.c src/json.c src/util.c

build/test_cli: tests/test_cli.c src/main.c $(LIB_SRCS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSKBD_TEST -o $@ tests/test_cli.c src/main.c $(LIB_SRCS)

build/test_runtime: tests/test_runtime.c src/main.c $(LIB_SRCS) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DSKBD_TEST -o $@ tests/test_runtime.c src/main.c $(LIB_SRCS)

test: all
	./build/test_memory
	./build/test_config
	./build/test_json
	./build/test_ipc
	./build/test_cli
	./build/test_runtime

clean:
	rm -rf bin build
