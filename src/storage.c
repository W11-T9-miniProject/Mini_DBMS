#include "storage.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const unsigned char FILE_MAGIC[8] = {'M', 'I', 'N', 'I', 'D', 'B', '0', '1'};

static void encode_u32(unsigned char *bytes, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

static void encode_u64(unsigned char *bytes, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) bytes[i] = (unsigned char)(value >> (8 * i));
}

static uint32_t decode_u32(const unsigned char *bytes) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}

static uint64_t decode_u64(const unsigned char *bytes) {
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= (uint64_t)bytes[i] << (8 * i);
    return value;
}

/* FNV-1a detects accidental record corruption; it is not cryptographic. */
static uint32_t record_checksum(const unsigned char *bytes, size_t length) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < length; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static void encode_record(unsigned char *bytes, const DbRow *row) {
    encode_u64(bytes, row->id);
    encode_u32(bytes + 8, (uint32_t)row->age);
    memcpy(bytes + 12, row->name, DB_NAME_SIZE);
    memcpy(bytes + 76, row->email, DB_EMAIL_SIZE);
    encode_u32(bytes + 204, record_checksum(bytes, 204));
}

static DbStatus decode_record(const unsigned char *bytes, DbRow *row, DbError *error) {
    if (decode_u32(bytes + 204) != record_checksum(bytes, 204))
        return db_error_set(error, DB_CORRUPT, "Record checksum mismatch");
    row->id = decode_u64(bytes);
    uint32_t age = decode_u32(bytes + 8);
    row->age = age <= INT32_MAX ? (int32_t)age : (int32_t)((int64_t)age - 4294967296LL);
    memcpy(row->name, bytes + 12, DB_NAME_SIZE);
    memcpy(row->email, bytes + 76, DB_EMAIL_SIZE);
    if (row->id == 0 || row->id > INT64_MAX ||
        !memchr(row->name, 0, DB_NAME_SIZE) || !memchr(row->email, 0, DB_EMAIL_SIZE))
        return db_error_set(error, DB_CORRUPT, "Invalid ID or unterminated stored string");
    return DB_OK;
}

uint64_t storage_row_offset(uint64_t row_number) {
    return STORAGE_HEADER_BYTES + row_number * STORAGE_RECORD_BYTES;
}

static DbStatus initialize_or_validate_header(Storage *storage, uint64_t size, DbError *error) {
    unsigned char header[STORAGE_HEADER_BYTES] = {0};
    if (size == 0) {
        memcpy(header, FILE_MAGIC, 8);
        encode_u32(header + 8, 1);
        encode_u32(header + 12, STORAGE_RECORD_BYTES);
        if (fwrite(header, 1, sizeof(header), storage->file) != sizeof(header) ||
            fflush(storage->file) != 0 || (storage->sync_writes && !platform_sync(storage->file)))
            return db_error_set(error, DB_IO_ERROR, "Cannot write database header");
        return DB_OK;
    }
    if (size < sizeof(header) || fread(header, 1, sizeof(header), storage->file) != sizeof(header))
        return db_error_set(error, DB_CORRUPT, "Truncated database header");
    if (memcmp(header, FILE_MAGIC, 8) != 0 || decode_u32(header + 8) != 1 ||
        decode_u32(header + 12) != STORAGE_RECORD_BYTES)
        return db_error_set(error, DB_CORRUPT, "Unsupported database header or version");
    return DB_OK;
}

DbStatus storage_open(Storage *storage, const char *directory, bool sync_writes, DbError *error) {
    memset(storage, 0, sizeof(*storage));
    storage->sync_writes = sync_writes;
    char file_path[1024], lock_path[1024];
    if (!directory || !directory[0] || strlen(directory) > 980) {
        db_error_set(error, DB_IO_ERROR, "Invalid or too long database directory");
        goto fail;
    }
    if (!platform_create_directory(directory)) {
        db_error_set(error, DB_IO_ERROR, "Cannot create/open database directory: %s", strerror(errno));
        goto fail;
    }
    snprintf(file_path, sizeof(file_path), "%s/users.db", directory);
    snprintf(lock_path, sizeof(lock_path), "%s/users.lock", directory);
    if (!platform_lock_file(lock_path, &storage->process_lock)) {
        db_error_set(error, DB_BUSY, "Database is already open or lock file is inaccessible");
        goto fail;
    }
    storage->process_lock_acquired = true;
    storage->file = fopen(file_path, "r+b");
    if (!storage->file && errno == ENOENT) storage->file = fopen(file_path, "w+b");
    if (!storage->file) {
        db_error_set(error, DB_IO_ERROR, "Cannot open users.db: %s", strerror(errno));
        goto fail;
    }
    setvbuf(storage->file, NULL, _IOFBF, 1024 * 1024);
    uint64_t size;
    if (!platform_file_size(storage->file, &size)) {
        db_error_set(error, DB_IO_ERROR, "Cannot inspect database size");
        goto fail;
    }
    if (initialize_or_validate_header(storage, size, error) != DB_OK) goto fail;
    if (size == 0) size = STORAGE_HEADER_BYTES;
    /* Fail closed on partial writes. Never discard user bytes automatically. */
    if ((size - STORAGE_HEADER_BYTES) % STORAGE_RECORD_BYTES != 0) {
        db_error_set(error, DB_CORRUPT, "Incomplete final record; restore a backup (file was not modified)");
        goto fail;
    }
    storage->row_count = (size - STORAGE_HEADER_BYTES) / STORAGE_RECORD_BYTES;
    if (!platform_reader_open(file_path, &storage->reader)) {
        db_error_set(error, DB_IO_ERROR, "Cannot open positional reader");
        goto fail;
    }
    storage->reader_open = true;
    return DB_OK;
fail:
    storage_close(storage);
    return error ? error->code : DB_IO_ERROR;
}

void storage_close(Storage *storage) {
    if (storage->reader_open) platform_reader_close(storage->reader);
    if (storage->file) fclose(storage->file);
    if (storage->process_lock_acquired) platform_unlock_file(storage->process_lock);
    storage->file = NULL;
    storage->process_lock_acquired = false;
    storage->reader_open = false;
}

DbStatus storage_read(Storage *storage, uint64_t offset, DbRow *row, DbError *error) {
    unsigned char bytes[STORAGE_RECORD_BYTES];
    bool success = platform_read_at(storage->reader, offset, bytes, sizeof(bytes));
    if (!success) return db_error_set(error, DB_IO_ERROR, "Cannot read record at offset %llu", (unsigned long long)offset);
    return decode_record(bytes, row, error);
}

DbStatus storage_read_batch(Storage *storage, uint64_t offset, DbRow *rows, size_t count, DbError *error) {
    if (count == 0 || count > STORAGE_READ_BATCH_ROWS)
        return db_error_set(error, DB_IO_ERROR, "Invalid read batch size");
    unsigned char bytes[STORAGE_READ_BATCH_ROWS * STORAGE_RECORD_BYTES];
    if (!platform_read_at(storage->reader, offset, bytes, count * STORAGE_RECORD_BYTES))
        return db_error_set(error, DB_IO_ERROR, "Cannot read batch at offset %llu", (unsigned long long)offset);
    for (size_t i = 0; i < count; ++i) {
        DbStatus status = decode_record(bytes + i * STORAGE_RECORD_BYTES, &rows[i], error);
        if (status != DB_OK) return status;
    }
    return DB_OK;
}

static void rollback_failed_append(Storage *storage, uint64_t original_size) {
    /* A failed write may have buffered bytes. Flush before truncating them away. */
    fflush(storage->file);
    clearerr(storage->file);
    bool restored = platform_truncate(storage->file, original_size);
    restored = platform_seek(storage->file, original_size) && restored;
    if (storage->sync_writes) restored = platform_sync(storage->file) && restored;
    storage->failed = !restored;
}

DbStatus storage_append(Storage *storage, DbRow *rows, size_t count, DbError *error) {
    if (count == 0 || count > 4096 || storage->row_count > (uint64_t)INT64_MAX / STORAGE_RECORD_BYTES - count)
        return db_error_set(error, DB_IO_ERROR, "Invalid batch size or database size limit reached");
    unsigned char *bytes = malloc(count * STORAGE_RECORD_BYTES);
    if (!bytes) return db_error_set(error, DB_MEMORY_ERROR, "Cannot allocate record buffer");
    for (size_t i = 0; i < count; ++i) {
        rows[i].id = storage->row_count + i + 1;
        encode_record(bytes + i * STORAGE_RECORD_BYTES, &rows[i]);
    }
    uint64_t original_size = storage_row_offset(storage->row_count);
    /* One writer owns this stream. Readers use an independent positional handle. */
    bool success = platform_seek(storage->file, original_size) &&
                   fwrite(bytes, STORAGE_RECORD_BYTES, count, storage->file) == count &&
                   fflush(storage->file) == 0;
    if (success && storage->sync_writes) success = platform_sync(storage->file);
    free(bytes);
    if (!success) {
        rollback_failed_append(storage, original_size);
        return db_error_set(error, DB_IO_ERROR, "Append failed%s", storage->failed ? "; close and reopen database" : "; write rolled back");
    }
    storage->row_count += count;
    return DB_OK;
}
