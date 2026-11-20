#ifndef MINI_STORAGE_H
#define MINI_STORAGE_H

#include "internal.h"
#include "platform.h"

#define STORAGE_HEADER_BYTES 16u
#define STORAGE_RECORD_BYTES 208u
#define STORAGE_READ_BATCH_ROWS 256u

typedef struct {
    FILE *file;
    PlatformReader reader;
    PlatformFileLock process_lock;
    bool process_lock_acquired;
    bool reader_open;
    bool sync_writes;
    bool failed;
    uint64_t row_count;
} Storage;

DbStatus storage_open(Storage *storage, const char *directory, bool sync_writes, DbError *error);
void storage_close(Storage *storage);
DbStatus storage_read(Storage *storage, uint64_t offset, DbRow *row, DbError *error);
DbStatus storage_read_batch(Storage *storage, uint64_t offset, DbRow *rows, size_t count, DbError *error);
DbStatus storage_append(Storage *storage, DbRow *rows, size_t count, DbError *error);
uint64_t storage_row_offset(uint64_t row_number);

#endif
