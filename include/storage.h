#ifndef MINI_DBMS_STORAGE_H
#define MINI_DBMS_STORAGE_H

#include <stdint.h>

#define DB_HEADER_SIZE UINT64_C(32)
#define DB_RECORD_SIZE UINT64_C(92)

typedef enum {
    DB_OK, DB_NOT_FOUND, DB_INVALID_ARGUMENT, DB_IO_ERROR,
    DB_CORRUPT_FILE, DB_LOCKED, DB_NO_MEMORY, DB_LIMIT
} db_status;

typedef struct {
    uint64_t id;
    uint64_t user_id;
    uint64_t song_id;
    char event_code[32];
    char genre[32];
} play_record;

typedef struct db_file db_file;

/* Thread synchronization belongs to the future engine; one file owner for now. */
db_status db_open(db_file **out, const char *path);
/* Atomically refuses existing paths; used for isolated benchmark datasets. */
db_status db_create_new(db_file **out, const char *path);
/* Input id must be zero. A successful append does not imply fsync completion. */
db_status db_append(db_file *file, const play_record *input,
                    play_record *saved, uint64_t *offset);
db_status db_read_at(db_file *file, uint64_t offset, play_record *out);
uint64_t db_count(const db_file *file);
db_status db_sync(db_file *file);
/* Always releases the handle, including when sync fails. */
db_status db_close(db_file *file);
const char *db_status_text(db_status status);

#endif
