#include "db.h"
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s DATABASE_DIRECTORY SQL_FILE\n", argv[0]);
        return 1;
    }
    FILE *file = fopen(argv[2], "rb");
    if (!file) { perror(argv[2]); return 1; }
    size_t capacity = 4096, length = 0;
    char *sql = malloc(capacity);
    if (!sql) { fclose(file); fprintf(stderr, "Out of memory\n"); return 1; }
    int c;
    while ((c = fgetc(file)) != EOF) {
        if (length + 1 == capacity) {
            if (capacity > SIZE_MAX / 2) { free(sql); fclose(file); return 1; }
            capacity *= 2;
            char *next = realloc(sql, capacity);
            if (!next) { free(sql); fclose(file); return 1; }
            sql = next;
        }
        if (c == 0) {
            fprintf(stderr, "SQL file contains a NUL byte\n");
            free(sql); fclose(file); return 1;
        }
        sql[length++] = (char)c;
    }
    int read_failed = ferror(file);
    fclose(file);
    if (read_failed) { free(sql); fprintf(stderr, "Cannot read SQL file\n"); return 1; }
    sql[length] = '\0';
    char error[512];
    int result = db_execute(argv[1], sql, stdout, error, sizeof(error));
    free(sql);
    if (result) fprintf(stderr, "%s\n", error);
    if (fflush(stdout) == EOF) { fprintf(stderr, "Cannot write output\n"); return 1; }
    return result ? 1 : 0;
}
