CC = cc
CPPFLAGS += -Iinclude -Isrc -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -pthread
CORE = src/abi.c src/error.c src/platform.c src/bptree.c src/parser.c src/storage.c src/engine.c src/script.c
PYTHON ?= python3
SHARED_EXT = so
SHARED_FLAGS = -shared -fPIC
ifeq ($(shell uname -s),Darwin)
SHARED_EXT = dylib
SHARED_FLAGS = -dynamiclib -fPIC
endif
HEADERS = $(wildcard include/*.h src/*.h)

.PHONY: all test benchmark sanitize api-test
all: build/mini_db build/db_tests build/db_bench build/db_load build/libminidb.$(SHARED_EXT)

build:
	mkdir -p build

build/mini_db: $(CORE) src/main.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) src/main.c -o $@ $(LDLIBS)

build/db_tests: $(CORE) tests/test_main.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tests/test_main.c -o $@ $(LDLIBS)

build/db_bench: $(CORE) tools/benchmark.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/benchmark.c -o $@ $(LDLIBS)

build/db_load: $(CORE) tools/load_benchmark.c $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CORE) tools/load_benchmark.c -o $@ $(LDLIBS)

build/libminidb.$(SHARED_EXT): $(CORE) $(HEADERS) | build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SHARED_FLAGS) $(CORE) -o $@ $(LDLIBS)

api-test: build/libminidb.$(SHARED_EXT)
	$(PYTHON) -m unittest discover -s tests -p "test_api*.py" -v

test: build/db_tests
	./build/db_tests

benchmark: build/db_bench
	./build/db_bench build/benchmark-data 1000000 30

sanitize: | build
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer $(CORE) tests/test_main.c -o build/db_tests_sanitize $(LDLIBS)
	./build/db_tests_sanitize
