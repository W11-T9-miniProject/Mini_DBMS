/* A competing writer creates the target after benchmark's preliminary lstat. */
#include "engine.h"
#include <fcntl.h>
#include <unistd.h>

db_status __real_engine_create_new(db_engine **out, const char *path);
db_status __wrap_engine_create_new(db_engine **out, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return DB_IO_ERROR;
    /* An existing empty file is accepted and initialized by ordinary db_open.
       This distinguishes exclusive creation from corruption rejection. */
    if (close(fd)) return DB_IO_ERROR;
    return __real_engine_create_new(out, path);
}
