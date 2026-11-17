CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -pthread
CPPFLAGS = -D_POSIX_C_SOURCE=200809L

all: mini_dbms mini_dbms_server

mini_dbms: src/main.c src/engine.c src/storage.c src/bptree.c src/db.h src/bptree.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/main.c src/engine.c src/storage.c src/bptree.c -o $@ $(LDLIBS)

test_bptree: tests/test_bptree.c src/bptree.c src/bptree.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBPTREE_TESTING -Isrc tests/test_bptree.c src/bptree.c -o $@ $(LDLIBS)


mini_dbms_server: src/server.c src/engine.c src/storage.c src/bptree.c src/db.h src/bptree.h
	$(CC) $(CPPFLAGS) $(CFLAGS) src/server.c src/engine.c src/storage.c src/bptree.c -o $@ $(LDLIBS)

test_engine: tests/test_engine.c src/engine.c src/storage.c src/bptree.c src/db.h src/bptree.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DBPTREE_TESTING -Isrc tests/test_engine.c src/engine.c src/storage.c src/bptree.c -o $@ $(LDLIBS)

test: mini_dbms mini_dbms_server test_bptree test_engine
	./test_bptree
	./test_engine
	python3 -m unittest discover -s tests -v

bench: mini_dbms
	python3 bench/phase2_benchmark.py

clean:
	rm -f mini_dbms mini_dbms_server test_bptree test_engine

.PHONY: all test bench clean
