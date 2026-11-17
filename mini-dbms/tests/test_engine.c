#include "db.h"
#include "bptree.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "engine check failed at line %d\n", __LINE__); return 1; } } while (0)

int main(void) {
    char directory[128];
    snprintf(directory, sizeof(directory), "/tmp/mini-db-engine-%ld", (long)getpid());
    CHECK(mkdir(directory, 0700) == 0);
    char path[256], error[512];
    snprintf(path, sizeof(path), "%s/people.schema", directory);
    FILE *schema = fopen(path, "w");
    CHECK(schema);
    CHECK(fputs("id INTEGER\nname TEXT\n", schema) >= 0);
    CHECK(fclose(schema) == 0);
    DbEngine *engine = db_engine_open(directory, error, sizeof(error));
    CHECK(engine);
    FILE *output = tmpfile();
    CHECK(output);
    for (int id = 1; id <= 31; ++id) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO people VALUES (%d, 'seed');", id);
        CHECK(db_engine_execute(engine, sql, output, error, sizeof(error)) == 0);
    }
    snprintf(path, sizeof(path), "%s/people.rows", directory);
    struct stat before, after;
    CHECK(stat(path, &before) == 0);
    bptree_test_fail_after(1);
    CHECK(db_engine_execute(engine, "INSERT INTO people VALUES (32, 'fail');", output, error, sizeof(error)) == -1);
    CHECK(strstr(error, "Out of memory"));
    bptree_test_fail_after(-1);
    CHECK(stat(path, &after) == 0 && before.st_size == after.st_size);
    CHECK(db_engine_execute(engine, "SELECT * FROM people;", output, error, sizeof(error)) == -1);
    CHECK(strstr(error, "requires restart"));
    db_engine_close(engine);
    engine = db_engine_open(directory, error, sizeof(error));
    CHECK(engine);
    CHECK(db_engine_execute(engine, "SELECT * FROM people WHERE id = 32;", output, error, sizeof(error)) == 0);
    CHECK(ftell(output) == 0);
    CHECK(db_engine_execute(engine, "SELECT * FROM people;", output, error, sizeof(error)) == 0);
    rewind(output);
    int lines = 0, character;
    while ((character = fgetc(output)) != EOF) if (character == '\n') ++lines;
    CHECK(lines == 31);
    fclose(output); db_engine_close(engine);
    unlink(path);
    snprintf(path, sizeof(path), "%s/people.schema", directory); unlink(path);
    snprintf(path, sizeof(path), "%s/.engine.lock", directory); unlink(path);
    CHECK(rmdir(directory) == 0);
    return 0;
}
