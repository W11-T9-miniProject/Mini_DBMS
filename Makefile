CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64 -Iinclude
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -g -pthread
WRAPS = -Wl,--wrap=calloc -Wl,--wrap=pwrite64 -Wl,--wrap=fsync

.PHONY: test sanitize storage-demo parser-test cli benchmark server http-test http-sanitize
.DEFAULT_GOAL := test

cli: build/mini-dbms

ENGINE_SOURCES = src/engine.c src/sql.c src/storage.c src/bptree.c
ENGINE_HEADERS = include/engine.h include/sql.h include/storage.h include/bptree.h

build/test_engine: tests/test_engine.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_engine.c $(ENGINE_SOURCES) -Wl,--wrap=calloc -o $@

build/test_engine_sanitize: tests/test_engine.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_engine.c $(ENGINE_SOURCES) -Wl,--wrap=calloc -o $@

build/mini-dbms: src/main.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c $(ENGINE_SOURCES) -o $@

build/mini-dbms-sanitize: src/main.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer src/main.c $(ENGINE_SOURCES) -o $@

parser-test: build/test_sql
	./build/test_sql

build/test_sql: tests/test_sql.c src/sql.c include/sql.h include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_sql.c src/sql.c -o $@

build/test_sql_sanitize: tests/test_sql.c src/sql.c include/sql.h include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_sql.c src/sql.c -o $@

test: build/test_storage build/storage-demo build/test_sql build/test_engine build/mini-dbms build/test_bptree build/benchmark build/benchmark-race build/test_concurrency build/test_durability build/test_pool
	./build/test_pool
	./build/test_durability
	./build/test_concurrency
	./build/test_bptree
	./build/test_storage
	python3 tests/test_storage_process.py
	./build/test_sql
	./build/test_engine
	python3 tests/test_cli.py
	python3 tests/test_benchmark.py

sanitize: build/test_storage_sanitize build/storage-demo-sanitize build/test_sql_sanitize build/test_engine_sanitize build/mini-dbms-sanitize build/test_bptree_sanitize build/benchmark-sanitize build/benchmark-race-sanitize build/test_concurrency_sanitize build/test_durability_sanitize build/test_pool_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_pool_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_durability_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_concurrency_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_bptree_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_storage_sanitize
	MINI_DBMS_DEMO=build/storage-demo-sanitize ASAN_OPTIONS=detect_leaks=1 python3 tests/test_storage_process.py
	ASAN_OPTIONS=detect_leaks=1 ./build/test_sql_sanitize
	ASAN_OPTIONS=detect_leaks=1 ./build/test_engine_sanitize
	MINI_DBMS_CLI=build/mini-dbms-sanitize ASAN_OPTIONS=detect_leaks=1 python3 tests/test_cli.py
	MINI_DBMS_BENCHMARK=build/benchmark-sanitize MINI_DBMS_BENCHMARK_RACE=build/benchmark-race-sanitize ASAN_OPTIONS=detect_leaks=1 python3 tests/test_benchmark.py

storage-demo: build/storage-demo

build/test_storage: tests/test_storage.c src/storage.c include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_storage.c src/storage.c $(WRAPS) -o $@

build/test_storage_sanitize: tests/test_storage.c src/storage.c include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_storage.c src/storage.c $(WRAPS) -o $@

build/storage-demo: tools/storage_demo.c src/storage.c include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tools/storage_demo.c src/storage.c -o $@

build/storage-demo-sanitize: tools/storage_demo.c src/storage.c include/storage.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tools/storage_demo.c src/storage.c -o $@

build:
	mkdir -p build

build/test_bptree: tests/test_bptree.c src/bptree.c include/bptree.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_bptree.c src/bptree.c -Wl,--wrap=calloc -o $@

build/test_bptree_sanitize: tests/test_bptree.c src/bptree.c include/bptree.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_bptree.c src/bptree.c -Wl,--wrap=calloc -o $@

benchmark: build/benchmark

build/benchmark: tools/benchmark.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O2 tools/benchmark.c $(ENGINE_SOURCES) -o $@

build/benchmark-sanitize: tools/benchmark.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tools/benchmark.c $(ENGINE_SOURCES) -o $@

build/benchmark-race: tests/benchmark_race.c tools/benchmark.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -O2 tests/benchmark_race.c tools/benchmark.c $(ENGINE_SOURCES) -Wl,--wrap=engine_create_new -o $@

build/benchmark-race-sanitize: tests/benchmark_race.c tools/benchmark.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/benchmark_race.c tools/benchmark.c $(ENGINE_SOURCES) -Wl,--wrap=engine_create_new -o $@

build/test_concurrency: tests/test_concurrency.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_concurrency.c $(ENGINE_SOURCES) -o $@

build/test_concurrency_sanitize: tests/test_concurrency.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_concurrency.c $(ENGINE_SOURCES) -o $@

SERVER_SOURCES = src/server.c src/pool.c third_party/picohttpparser/picohttpparser.c $(ENGINE_SOURCES)
SERVER_HEADERS = include/pool.h third_party/picohttpparser/picohttpparser.h $(ENGINE_HEADERS)
server: build/mini-dbms-server

build/mini-dbms-server: $(SERVER_SOURCES) $(SERVER_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) -Ithird_party/picohttpparser $(CFLAGS) $(SERVER_SOURCES) -o $@

build/mini-dbms-server-sanitize: $(SERVER_SOURCES) $(SERVER_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) -Ithird_party/picohttpparser $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer $(SERVER_SOURCES) -o $@

http-test: build/mini-dbms-server
	python3 tests/test_http.py

http-sanitize: build/mini-dbms-server-sanitize
	MINI_DBMS_SERVER=build/mini-dbms-server-sanitize ASAN_OPTIONS=detect_leaks=1 python3 tests/test_http.py

build/test_durability: tests/test_durability.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_durability.c $(ENGINE_SOURCES) -Wl,--wrap=fsync -o $@

build/test_durability_sanitize: tests/test_durability.c $(ENGINE_SOURCES) $(ENGINE_HEADERS) Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_durability.c $(ENGINE_SOURCES) -Wl,--wrap=fsync -o $@

build/test_pool: tests/test_pool.c src/pool.c include/pool.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_pool.c src/pool.c -Wl,--wrap=calloc -Wl,--wrap=pthread_create -o $@

build/test_pool_sanitize: tests/test_pool.c src/pool.c include/pool.h Makefile | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer tests/test_pool.c src/pool.c -Wl,--wrap=calloc -Wl,--wrap=pthread_create -o $@
