#include "storage.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdatomic.h>

struct db_file {
    int fd;
    atomic_int failed;
    uint64_t count;
};

static const unsigned char magic[8] = {'M', 'J', 'D', 'B', 'M', 'S', '1', 0};

static void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static uint32_t get32(const unsigned char *p) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)p[i] << (i * 8);
    return value;
}
static void put64(unsigned char *p, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) p[i] = (unsigned char)(value >> (i * 8));
}
static uint64_t get64(const unsigned char *p) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)p[i] << (i * 8);
    return value;
}
static uint32_t checksum(const unsigned char *data, size_t size) {
    uint32_t hash = UINT32_C(2166136261);
    for (size_t i = 0; i < size; ++i) { hash ^= data[i]; hash *= UINT32_C(16777619); }
    return hash;
}

static db_status read_exact(int fd, void *buffer, size_t size, off_t offset) {
    unsigned char *p = buffer;
    while (size) {
        ssize_t n = pread(fd, p, size, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return DB_IO_ERROR;
        if (n == 0) return DB_CORRUPT_FILE;
        p += n; offset += n; size -= (size_t)n;
    }
    return DB_OK;
}
static db_status write_exact(int fd, const void *buffer, size_t size, off_t offset) {
    const unsigned char *p = buffer;
    while (size) {
        ssize_t n = pwrite(fd, p, size, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return DB_IO_ERROR;
        p += n; offset += n; size -= (size_t)n;
    }
    return DB_OK;
}
static db_status sync_fd(int fd) {
    int result;
    do { result = fsync(fd); } while (result < 0 && errno == EINTR);
    return result == 0 ? DB_OK : DB_IO_ERROR;
}

static int valid_text(const char *text, size_t capacity) {
    size_t size = strnlen(text, capacity);
    if (size == 0 || size == capacity) return 0;
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126) return 0;
    }
    return 1;
}
static int valid_fields(const play_record *record) {
    return record && record->user_id != 0 && record->song_id != 0 &&
        valid_text(record->event_code, sizeof(record->event_code)) &&
        valid_text(record->genre, sizeof(record->genre));
}

static void encode_record(unsigned char bytes[92], const play_record *record) {
    memset(bytes, 0, 92);
    put64(bytes, record->id);
    put64(bytes + 8, record->user_id);
    put64(bytes + 16, record->song_id);
    memcpy(bytes + 24, record->event_code, strlen(record->event_code));
    memcpy(bytes + 56, record->genre, strlen(record->genre));
    put32(bytes + 88, checksum(bytes, 88));
}
static int zero_padding(const unsigned char *p, size_t size) {
    size_t end = 0;
    while (end < size && p[end]) end++;
    if (end == size) return 0;
    for (; end < size; ++end) if (p[end]) return 0;
    return 1;
}
static db_status decode_record(const unsigned char bytes[92], play_record *out) {
    if (get32(bytes + 88) != checksum(bytes, 88) ||
        !zero_padding(bytes + 24, 32) || !zero_padding(bytes + 56, 32)) return DB_CORRUPT_FILE;
    play_record record = {0};
    record.id = get64(bytes);
    record.user_id = get64(bytes + 8);
    record.song_id = get64(bytes + 16);
    memcpy(record.event_code, bytes + 24, 32);
    memcpy(record.genre, bytes + 56, 32);
    if (record.id == 0 || !valid_fields(&record)) return DB_CORRUPT_FILE;
    *out = record;
    return DB_OK;
}

static db_status check_existing(db_file *file, off_t size) {
    if (size < (off_t)DB_HEADER_SIZE || ((uint64_t)size - DB_HEADER_SIZE) % DB_RECORD_SIZE) return DB_CORRUPT_FILE;
    unsigned char header[32];
    db_status status = read_exact(file->fd, header, sizeof(header), 0);
    if (status != DB_OK) return status;
    if (memcmp(header, magic, 8) || get32(header + 8) != 2 || get32(header + 12) != 92 ||
        get32(header + 28) != checksum(header, 28)) return DB_CORRUPT_FILE;
    for (size_t i = 16; i < 28; ++i) if (header[i] != 0) return DB_CORRUPT_FILE;
    uint64_t count = ((uint64_t)size - DB_HEADER_SIZE) / DB_RECORD_SIZE;
    unsigned char block[92 * 128];
    for (uint64_t index = 0; index < count;) {
        size_t rows = count - index > 128 ? 128 : (size_t)(count - index);
        status = read_exact(file->fd, block, rows * 92, (off_t)(DB_HEADER_SIZE + index * DB_RECORD_SIZE));
        if (status != DB_OK) return status;
        for (size_t i = 0; i < rows; ++i) {
            play_record record;
            status = decode_record(block + i * 92, &record);
            if (status != DB_OK) return status;
            if (record.id != index + i + 1) return DB_CORRUPT_FILE;
        }
        index += rows;
    }
    file->count = count;
    return DB_OK;
}

static db_status open_file(db_file **out, const char *path, int exclusive_create) {
    if (!out) return DB_INVALID_ARGUMENT;
    *out = NULL;
    if (!path || !*path) return DB_INVALID_ARGUMENT;
    db_file *file = calloc(1, sizeof(*file));
    if (!file) return DB_NO_MEMORY;
    atomic_init(&file->failed, 0);
    file->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | (exclusive_create ? O_EXCL : 0), 0600);
    if (file->fd < 0) { free(file); return DB_IO_ERROR; }
    if (flock(file->fd, LOCK_EX | LOCK_NB) < 0) {
        db_status status = (errno == EWOULDBLOCK || errno == EAGAIN) ? DB_LOCKED : DB_IO_ERROR;
        close(file->fd); free(file); return status;
    }
    struct stat info;
    db_status status = DB_OK;
    if (fstat(file->fd, &info) < 0) status = DB_IO_ERROR;
    else if (!S_ISREG(info.st_mode)) status = DB_INVALID_ARGUMENT;
    else if (info.st_size == 0) {
        unsigned char header[32] = {0};
        memcpy(header, magic, 8); put32(header + 8, 2); put32(header + 12, 92);
        put32(header + 28, checksum(header, 28));
        status = write_exact(file->fd, header, sizeof(header), 0);
        if (status == DB_OK) status = sync_fd(file->fd);
    } else status = check_existing(file, info.st_size);
    if (status != DB_OK) { close(file->fd); free(file); return status; }
    *out = file;
    return DB_OK;
}

db_status db_open(db_file **out, const char *path) { return open_file(out, path, 0); }
db_status db_create_new(db_file **out, const char *path) { return open_file(out, path, 1); }

db_status db_append(db_file *file, const play_record *input, play_record *saved, uint64_t *offset) {
    if (!file || !input || input->id != 0 || !valid_fields(input)) return DB_INVALID_ARGUMENT;
    if (file->failed) return DB_IO_ERROR;
    if (file->count >= ((uint64_t)INT64_MAX - DB_HEADER_SIZE) / DB_RECORD_SIZE) return DB_LIMIT;
    play_record record = {0};
    record.id = file->count + 1; record.user_id = input->user_id; record.song_id = input->song_id;
    memcpy(record.event_code, input->event_code, strlen(input->event_code));
    memcpy(record.genre, input->genre, strlen(input->genre));
    unsigned char bytes[92]; encode_record(bytes, &record);
    uint64_t position = DB_HEADER_SIZE + file->count * DB_RECORD_SIZE;
    db_status status = write_exact(file->fd, bytes, sizeof(bytes), (off_t)position);
    if (status != DB_OK) { file->failed = 1; return status; }
    file->count++;
    if (saved) *saved = record;
    if (offset) *offset = position;
    return DB_OK;
}

db_status db_read_at(db_file *file, uint64_t offset, play_record *out) {
    if (!file || !out || offset < DB_HEADER_SIZE || (offset - DB_HEADER_SIZE) % DB_RECORD_SIZE) return DB_INVALID_ARGUMENT;
    uint64_t index = (offset - DB_HEADER_SIZE) / DB_RECORD_SIZE;
    if (index >= file->count) return DB_NOT_FOUND;
    unsigned char bytes[92];
    db_status status = read_exact(file->fd, bytes, sizeof(bytes), (off_t)offset);
    play_record record;
    if (status == DB_OK) status = decode_record(bytes, &record);
    if (status == DB_OK && record.id != index + 1) status = DB_CORRUPT_FILE;
    if (status != DB_OK) { file->failed = 1; return status; }
    *out = record;
    return DB_OK;
}

uint64_t db_count(const db_file *file) { return file ? file->count : 0; }

db_status db_sync(db_file *file) {
    if (!file) return DB_INVALID_ARGUMENT;
    if (file->failed) return DB_IO_ERROR;
    db_status status = sync_fd(file->fd);
    if (status != DB_OK) file->failed = 1;
    return status;
}

db_status db_close(db_file *file) {
    if (!file) return DB_INVALID_ARGUMENT;
    db_status status = db_sync(file);
    if (close(file->fd) < 0) status = DB_IO_ERROR;
    free(file);
    return status;
}

const char *db_status_text(db_status status) {
    switch (status) {
        case DB_OK: return "ok";
        case DB_NOT_FOUND: return "not found";
        case DB_INVALID_ARGUMENT: return "invalid argument";
        case DB_IO_ERROR: return "I/O failure; write outcome may be uncertain";
        case DB_CORRUPT_FILE: return "invalid or incomplete database file; file preserved";
        case DB_LOCKED: return "database file is already open";
        case DB_NO_MEMORY: return "out of memory";
        case DB_LIMIT: return "file size limit exceeded";
    }
    return "unknown error";
}
