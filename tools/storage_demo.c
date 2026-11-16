#include "storage.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "write") && strcmp(argv[1], "read"))) {
        fputs("Usage: storage-demo write|read <file>\n", stderr); return 2;
    }
    db_file *file = NULL;
    play_record saved = {0};
    uint64_t offset = 0;
    db_status status = db_open(&file, argv[2]);
    if (status != DB_OK) { fprintf(stderr, "%s\n", db_status_text(status)); return 1; }
    if (strcmp(argv[1], "write") == 0) {
        play_record input = { .user_id = 71, .song_id = 305 };
        strcpy(input.event_code, "PLAY000001");
        strcpy(input.genre, "rock");
        status = db_append(file, &input, &saved, &offset);
    } else {
        puts("id\tevent_code\tuser_id\tsong_id\tgenre");
        for (uint64_t i = 0; i < db_count(file); ++i) {
            play_record record;
            status = db_read_at(file, DB_HEADER_SIZE + i * DB_RECORD_SIZE, &record);
            if (status != DB_OK) break;
            printf("%" PRIu64 "\t%s\t%" PRIu64 "\t%" PRIu64 "\t%s\n", record.id,
                   record.event_code, record.user_id, record.song_id, record.genre);
        }
    }
    db_status closed = db_close(file);
    if (status == DB_OK) status = closed;
    if (status != DB_OK) { fprintf(stderr, "%s\n", db_status_text(status)); return 1; }
    if (strcmp(argv[1], "write") == 0) printf("SAVED id=%" PRIu64 " offset=%" PRIu64 "\n", saved.id, offset);
    return 0;
}
