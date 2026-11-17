#include "storage.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail_alloc, fail_sync, partial_write;
void *__real_calloc(size_t, size_t);
ssize_t __real_pwrite64(int, const void *, size_t, off_t);
int __real_fsync(int);

void *__wrap_calloc(size_t n, size_t size) {
    if (fail_alloc) { fail_alloc = 0; return NULL; }
    return __real_calloc(n, size);
}
ssize_t __wrap_pwrite64(int fd, const void *data, size_t size, off_t offset) {
    if (partial_write == 1) {
        partial_write = 2;
        return __real_pwrite64(fd, data, size > 7 ? 7 : size, offset);
    }
    if (partial_write == 2) { partial_write = 0; errno = ENOSPC; return -1; }
    return __real_pwrite64(fd, data, size, offset);
}
int __wrap_fsync(int fd) {
    if (fail_sync) { fail_sync = 0; errno = EIO; return -1; }
    return __real_fsync(fd);
}

static char path[256];

static play_record input(void) {
    play_record record = { .user_id = 71, .song_id = 305 };
    strcpy(record.event_code, "PLAY000001");
    strcpy(record.genre, "rock");
    return record;
}

static void fresh(void) { unlink(path); }
static off_t file_size(void) { struct stat st; assert(stat(path, &st) == 0); return st.st_size; }

static void test_round_trip(void) {
    fresh();
    db_file *file = NULL;
    assert(db_open(&file, path) == DB_OK);
    assert(db_count(file) == 0);
    play_record record = input(), saved, read;
    uint64_t offset = 0;
    assert(db_read_at(file, DB_HEADER_SIZE, &read) == DB_NOT_FOUND);
    assert(db_append(file, &record, &saved, &offset) == DB_OK);
    assert(saved.id == 1 && offset == DB_HEADER_SIZE);
    assert(db_read_at(file, offset, &read) == DB_OK);
    assert(read.id == 1 && read.user_id == 71 && read.song_id == 305);
    assert(strcmp(read.event_code, "PLAY000001") == 0);
    assert(strcmp(read.genre, record.genre) == 0);
    assert(db_close(file) == DB_OK);
    assert(file_size() == (off_t)(DB_HEADER_SIZE + DB_RECORD_SIZE));
    assert(db_open(&file, path) == DB_OK);
    assert(db_count(file) == 1);
    assert(db_append(file, &record, &saved, &offset) == DB_OK);
    assert(saved.id == 2 && offset == DB_HEADER_SIZE + DB_RECORD_SIZE);
    assert(db_close(file) == DB_OK);
}

static void test_format_and_owned_values(void) {
    fresh();
    db_file *file = NULL;
    assert(db_open(&file, path) == DB_OK);
    play_record record = input(), saved;
    assert(db_append(file, &record, &saved, NULL) == DB_OK);
    memset(&record, 0, sizeof(record));
    assert(db_read_at(file, DB_HEADER_SIZE, &saved) == DB_OK);
    assert(saved.user_id == 71);
    unsigned char bytes[124];
    int fd = open(path, O_RDONLY); assert(fd >= 0);
    assert(read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes)); close(fd);
    assert(memcmp(bytes, "MJDBMS1\0", 8) == 0);
    assert(bytes[8] == 2 && bytes[12] == 92);
    assert(bytes[32] == 1 && bytes[40] == 71);
    assert(bytes[48] == 49 && bytes[49] == 1); /* 305, little endian */
    assert(memcmp(bytes + 56, "PLAY000001", 10) == 0);
    assert(memcmp(bytes + 88, "rock", 5) == 0);
    assert(db_close(file) == DB_OK);
}

static void test_validation(void) {
    fresh(); db_file *file = NULL;
    assert(db_open(NULL, path) == DB_INVALID_ARGUMENT);
    assert(db_open(&file, NULL) == DB_INVALID_ARGUMENT);
    assert(db_open(&file, path) == DB_OK);
    play_record record = input(), output = { .id = 999 };
    uint64_t offset = 999;
    record.user_id = 0;
    assert(db_append(file, &record, &output, &offset) == DB_INVALID_ARGUMENT);
    assert(output.id == 999 && offset == 999 && db_count(file) == 0);
    record = input(); record.id = 1;
    assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    record = input(); memset(record.event_code, 'x', sizeof(record.event_code));
    assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    record = input(); strcpy(record.event_code, "bad\ncode");
    assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    record = input(); record.event_code[0] = 0;
    assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    const char *invalid_genres[] = {"", "bad\ngenre", "bad\tgenre", "\x7f", "\x80"};
    for (size_t i = 0; i < sizeof(invalid_genres) / sizeof(*invalid_genres); ++i) {
        record = input(); snprintf(record.genre, sizeof(record.genre), "%s", invalid_genres[i]);
        assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    }
    record = input(); memset(record.genre, 'X', sizeof(record.genre));
    assert(db_append(file, &record, &output, NULL) == DB_INVALID_ARGUMENT);
    record = input();
    memset(record.genre, 'G', 31); record.genre[31] = 0;
    memset(record.event_code, 'X', 31); record.event_code[31] = 0;
    assert(db_append(file, &record, &output, NULL) == DB_OK);
    assert(db_read_at(file, DB_HEADER_SIZE, &output) == DB_OK);
    assert(strcmp(output.genre, record.genre) == 0);
    assert(db_count(file) == 1);
    assert(db_read_at(file, 0, &output) == DB_INVALID_ARGUMENT);
    assert(db_read_at(file, DB_HEADER_SIZE + 1, &output) == DB_INVALID_ARGUMENT);
    assert(db_read_at(file, DB_HEADER_SIZE, NULL) == DB_INVALID_ARGUMENT);
    assert(db_close(file) == DB_OK);
}

static void test_many_records(void) {
    fresh(); db_file *file = NULL;
    assert(db_open(&file, path) == DB_OK);
    for (uint64_t i = 0; i < 1000; ++i) {
        play_record record = input(), saved; uint64_t offset;
        record.song_id = i + 1;
        assert(db_append(file, &record, &saved, &offset) == DB_OK);
        assert(saved.id == i + 1 && offset == DB_HEADER_SIZE + i * DB_RECORD_SIZE);
    }
    assert(db_close(file) == DB_OK);
    assert(db_open(&file, path) == DB_OK && db_count(file) == 1000);
    for (uint64_t i = 0; i < 1000; ++i) {
        play_record record;
        assert(db_read_at(file, DB_HEADER_SIZE + i * DB_RECORD_SIZE, &record) == DB_OK);
        assert(record.id == i + 1 && record.song_id == i + 1);
    }
    assert(db_close(file) == DB_OK);
}

static void create_one(void) {
    fresh(); db_file *file = NULL; play_record record = input();
    assert(db_open(&file, path) == DB_OK);
    assert(db_append(file, &record, NULL, NULL) == DB_OK);
    assert(db_close(file) == DB_OK);
}

static void test_corruption_preserves_file(void) {
    db_file *file = NULL;
    create_one(); off_t original_size = file_size();
    int fd = open(path, O_RDWR); assert(fd >= 0);
    unsigned char byte; assert(pread(fd, &byte, 1, 60) == 1);
    byte ^= 1; assert(pwrite(fd, &byte, 1, 60) == 1); close(fd);
    assert(db_open(&file, path) == DB_CORRUPT_FILE && file == NULL);
    assert(file_size() == original_size);
    create_one(); assert(truncate(path, original_size - 1) == 0);
    assert(db_open(&file, path) == DB_CORRUPT_FILE && file == NULL);
    assert(file_size() == original_size - 1);
    create_one(); fd = open(path, O_RDWR); assert(fd >= 0);
    byte = 'Z'; assert(pwrite(fd, &byte, 1, 0) == 1); close(fd);
    assert(db_open(&file, path) == DB_CORRUPT_FILE && file == NULL);
    assert(file_size() == original_size);
}

static void test_exclusive_open_and_allocation(void) {
    fresh(); db_file *first = NULL, *second = NULL;
    assert(db_create_new(&first, path) == DB_OK);
    assert(db_create_new(&second, path) == DB_IO_ERROR && second == NULL);
    assert(db_open(&second, path) == DB_LOCKED && second == NULL);
    assert(db_close(first) == DB_OK);
    off_t size = file_size();
    assert(db_create_new(&second, path) == DB_IO_ERROR && file_size() == size);
    fail_alloc = 1;
    assert(db_open(&first, path) == DB_NO_MEMORY && first == NULL);
    assert(db_open(&first, path) == DB_OK);
    assert(db_close(first) == DB_OK);
}

static void test_failed_write_blocks_append(void) {
    create_one(); db_file *file = NULL;
    assert(db_open(&file, path) == DB_OK);
    play_record record = input(), output = { .id = 999 }; uint64_t offset = 999;
    partial_write = 1;
    assert(db_append(file, &record, &output, &offset) == DB_IO_ERROR);
    assert(output.id == 999 && offset == 999 && db_count(file) == 1);
    assert(db_append(file, &record, NULL, NULL) == DB_IO_ERROR);
    assert(db_read_at(file, DB_HEADER_SIZE, &output) == DB_OK && output.id == 1);
    assert(db_close(file) == DB_IO_ERROR);
    off_t damaged_size = file_size();
    assert(db_open(&file, path) == DB_CORRUPT_FILE);
    assert(file_size() == damaged_size);
}

static void test_failed_sync(void) {
    fresh(); db_file *file = NULL; play_record record = input();
    assert(db_open(&file, path) == DB_OK);
    assert(db_append(file, &record, NULL, NULL) == DB_OK);
    fail_sync = 1;
    assert(db_sync(file) == DB_IO_ERROR);
    assert(db_append(file, &record, NULL, NULL) == DB_IO_ERROR);
    assert(db_read_at(file, DB_HEADER_SIZE, &record) == DB_OK);
    assert(db_close(file) == DB_IO_ERROR);
    /* Failed fsync does not prove the record was rolled back. */
    assert(db_open(&file, path) == DB_OK && db_count(file) == 1);
    assert(db_close(file) == DB_OK);
}

int main(void) {
    char directory[] = "/tmp/mini-dbms-storage-XXXXXX";
    assert(mkdtemp(directory));
    snprintf(path, sizeof(path), "%s/play_history.db", directory);
    test_round_trip(); puts("PASS append/read/reopen and automatic IDs");
    test_format_and_owned_values(); puts("PASS explicit 92-byte encoding and owned values");
    test_validation(); puts("PASS argument, string and genre validation");
    test_many_records(); puts("PASS 1000 records and exact offsets");
    test_corruption_preserves_file(); puts("PASS corrupt/incomplete files rejected without modification");
    test_exclusive_open_and_allocation(); puts("PASS exclusive ownership and allocation failure");
    test_failed_write_blocks_append(); puts("PASS partial write blocks further writes, keeps old reads");
    test_failed_sync(); puts("PASS sync failure is an uncertain outcome");
    unlink(path); assert(rmdir(directory) == 0);
    return 0;
}
