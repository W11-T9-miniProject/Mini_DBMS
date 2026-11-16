CC = cc
CPPFLAGS += -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -pthread
CORE = src/error.c src/platform.c src/bptree.c src/parser.c src/storage.c src/engine.c src/script.c
HEADERS = $(wildcard include/*.h src/*.h)

.PHONY: all test benchmark sanitize
all: build/mini_db build/db_tests build/db_bench

build:
	mkdir -p build

build/mini_db: $(CORE) src/main.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) src/main.c -o $@ $(LDLIBS)

build/db_tests: $(CORE) tests/test_main.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_main.c -o $@ $(LDLIBS)

build/db_bench: $(CORE) tools/benchmark.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/benchmark.c -o $@ $(LDLIBS)

test: build/db_tests
	./build/db_tests

benchmark: build/db_bench
	./build/db_bench build/benchmark-data 1000000 30

sanitize: | build
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(CORE) tests/test_main.c -o build/db_tests_sanitize $(LDLIBS)
	./build/db_tests_sanitize
