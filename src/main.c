#include "db.h"
#include "db_script.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Db *db;
    bool quiet;
    bool scan;
    bool header_printed;
    size_t statements;
} Cli;

static void print_csv_string(const char *value) {
    putchar('"');
    for (; *value; ++value) {
        if (*value == '"') putchar('"');
        putchar((unsigned char)*value);
    }
    putchar('"');
}

static bool print_result_row(const DbRow *row, const DbResult *result, void *context) {
    Cli *cli = context;
    if (!cli->header_printed) {
        for (size_t i = 0; i < result->column_count; ++i)
            printf("%s%s", i ? "," : "", result->columns[i]);
        putchar('\n');
        cli->header_printed = true;
    }
    for (size_t i = 0; i < result->column_count; ++i) {
        if (i) putchar(',');
        const char *column = result->columns[i];
        if (strcmp(column, "id") == 0) printf("%llu", (unsigned long long)row->id);
        else if (strcmp(column, "age") == 0) printf("%d", (int)row->age);
        else print_csv_string(strcmp(column, "name") == 0 ? row->name : row->email);
    }
    putchar('\n');
    return !ferror(stdout);
}

static DbStatus execute_cli_statement(const char *sql, void *context, DbError *error) {
    Cli *cli = context;
    DbResult result;
    cli->header_printed = false;
    ++cli->statements;
    DbRowVisitor visitor = cli->quiet ? NULL : print_result_row;
    DbStatus status = cli->scan
        ? db_execute_scan(cli->db, sql, visitor, cli, &result, error)
        : db_execute(cli->db, sql, visitor, cli, &result, error);
    if (status != DB_OK) return status;
    if (!cli->quiet) {
        if (result.plan == DB_PLAN_INSERT)
            printf("# INSERT: %llu rows, first_id=%llu\n", (unsigned long long)result.affected_rows,
                   (unsigned long long)result.first_insert_id);
        else printf("# %s: rows=%llu examined=%llu%s\n", db_plan_name(result.plan),
                    (unsigned long long)result.affected_rows, (unsigned long long)result.examined_rows,
                    result.explain ? " (EXPLAIN only)" : "");
    }
    return DB_OK;
}

static void print_usage(void) {
    puts("Mini DBMS (C11)\n"
         "Usage: mini_db [--db DIRECTORY] [--file FILE | --sql SQL] [--scan] [--quiet] [--no-sync]\n"
         "Without --file/--sql, reads SQL from standard input until EOF.\n"
         "  --db       Database directory (default: data; parent must exist)\n"
         "  --file     Execute a SQL file, stopping at the first error\n"
         "  --sql      Execute exactly one SQL statement\n"
         "  --scan     Disable index (for comparisons)\n"
         "  --quiet    Execute without printing rows or summaries\n"
         "  --no-sync  Flush to OS cache only; acknowledged writes may be lost on power failure\n"
         "Schema: users(id AUTO, name STRING[63 bytes], age INT32, email STRING[127 bytes] DEFAULT '')");
}

static int run_cli(int argc, char **argv) {
    const char *directory = "data", *file_path = NULL, *sql = NULL;
    bool sync_writes = true;
    Cli cli = {0};
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--help") == 0) { print_usage(); return 0; }
        if (strcmp(argv[i], "--quiet") == 0) cli.quiet = true;
        else if (strcmp(argv[i], "--scan") == 0) cli.scan = true;
        else if (strcmp(argv[i], "--no-sync") == 0) sync_writes = false;
        else if ((strcmp(argv[i], "--db") == 0 || strcmp(argv[i], "--file") == 0 ||
                  strcmp(argv[i], "--sql") == 0) && i + 1 < argc) {
            const char *option = argv[i++];
            if (strcmp(option, "--db") == 0) directory = argv[i];
            else if (strcmp(option, "--file") == 0) file_path = argv[i];
            else sql = argv[i];
        } else { fprintf(stderr, "Unknown option or missing value: %s\n", argv[i]); return 2; }
    }
    if (sql && file_path) { fputs("Use either --sql or --file\n", stderr); return 2; }
    FILE *input = stdin;
    if (file_path) {
        input = platform_open_file(file_path, "rb");
        if (!input) { perror(file_path); return 1; }
    }
    DbError error;
    cli.db = db_open(directory, sync_writes, &error);
    if (!cli.db) {
        fprintf(stderr, "%s: %s\n", db_status_name(error.code), error.message);
        if (file_path) fclose(input);
        return 1;
    }
    DbStatus status = sql ? execute_cli_statement(sql, &cli, &error)
                         : db_run_script(input, execute_cli_statement, &cli, &error);
    db_close(cli.db);
    if (file_path) fclose(input);
    if (status != DB_OK) {
        fprintf(stderr, "Statement %zu: %s: %s\n", cli.statements, db_status_name(status), error.message);
        return 1;
    }
    return ferror(stdout) ? 1 : 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **wide_argv) {
    char **argv = calloc((size_t)argc, sizeof(*argv));
    if (!argv) return 1;
    bool converted = true;
    for (int i = 0; i < argc; ++i) {
        int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, NULL, 0, NULL, NULL);
        if (bytes <= 0 || !(argv[i] = malloc((size_t)bytes)) ||
            !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide_argv[i], -1, argv[i], bytes, NULL, NULL)) {
            converted = false;
            break;
        }
    }
    SetConsoleOutputCP(CP_UTF8);
    int result = converted ? run_cli(argc, argv) : 1;
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return result;
}
#else
int main(int argc, char **argv) { return run_cli(argc, argv); }
#endif
