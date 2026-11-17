#include "sql.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static sql_statement parse(const char *text) {
    sql_parser parser; sql_statement statement; sql_error error;
    sql_parser_init(&parser, text, strlen(text));
    sql_status status = sql_next(&parser, &statement, &error);
    if (status != SQL_OK) fprintf(stderr, "%zu:%zu %s\n", error.line, error.column, error.message);
    assert(status == SQL_OK);
    assert(sql_next(&parser, &statement, &error) == SQL_DONE);
    return statement;
}

static void bad(const char *text) {
    sql_parser parser; sql_statement statement, original; sql_error error;
    memset(&statement, 0x55, sizeof(statement)); original = statement;
    sql_parser_init(&parser, text, strlen(text));
    assert(sql_next(&parser, &statement, &error) == SQL_ERROR);
    assert(memcmp(&statement, &original, sizeof(statement)) == 0);
    assert(error.line >= 1 && error.column >= 1 && error.message[0]);
}

static void test_insert(void) {
    sql_statement s = parse("INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('PLAY000001',71,305,'rock');");
    assert(s.kind == SQL_INSERT && s.record.id == 0);
    assert(s.record.user_id == 71 && s.record.song_id == 305);
    assert(strcmp(s.record.event_code, "PLAY000001") == 0);
    assert(strcmp(s.record.genre, "rock") == 0);
    s = parse("insert into PLAY_HISTORY (song_id,genre,event_code,user_id) values (305,'rock','P',71)");
    assert(s.record.user_id == 71 && s.record.song_id == 305);
    assert(strcmp(s.record.event_code, "P") == 0);
}

static void test_select(void) {
    sql_statement s = parse("SELECT * FROM play_history;");
    assert(s.kind == SQL_SELECT && !s.has_where);
    s = parse("select * from play_history WHERE id=18446744073709551615;");
    assert(s.has_where && s.field == SQL_ID && s.value.kind == SQL_NUMBER && s.value.number == UINT64_MAX);
    s = parse("SELECT * FROM play_history WHERE user_id = 71");
    assert(s.field == SQL_USER_ID && s.value.number == 71);
    s = parse("SELECT * FROM play_history WHERE song_id = 0;");
    assert(s.field == SQL_SONG_ID && s.value.number == 0);
    s = parse("SELECT * FROM play_history WHERE event_code = 'P'';--\\1';");
    assert(s.field == SQL_EVENT_CODE && strcmp(s.value.text, "P';--\\1") == 0);
    s = parse("SELECT * FROM play_history WHERE genre = 'rock';");
    assert(s.field == SQL_GENRE);
}

static void test_scripts_and_owned_values(void) {
    char text[] = "-- ignore ; ' \n;\nSELECT *\nFROM play_history; -- after\n SELECT * FROM play_history WHERE id=2;";
    sql_parser parser; sql_statement s; sql_error error;
    sql_parser_init(&parser, text, strlen(text));
    assert(sql_next(&parser, &s, &error) == SQL_OK && s.line == 3);
    assert(sql_next(&parser, &s, &error) == SQL_OK && s.value.number == 2);
    assert(sql_next(&parser, &s, &error) == SQL_DONE);
    char owned[] = "SELECT * FROM play_history WHERE event_code='retained';";
    sql_parser_init(&parser, owned, strlen(owned));
    assert(sql_next(&parser, &s, &error) == SQL_OK);
    memset(owned, 'X', sizeof(owned));
    assert(strcmp(s.value.text, "retained") == 0);
    sql_parser_init(&parser, "  -- comment", 12);
    assert(sql_next(&parser, &s, &error) == SQL_DONE);
}

static void test_bad_syntax_and_types(void) {
    const char *cases[] = {
        "CREATE TABLE play_history (id INT);", "DELETE FROM play_history;",
        "SELECT id FROM play_history;", "SELECT * FROM missing;",
        "SELECT * FROM play_history WHERE id='1';", "SELECT * FROM play_history WHERE event_code=1;",
        "SELECT * FROM play_history WHERE id=-1;", "SELECT * FROM play_history WHERE id=1.5;",
        "SELECT * FROM play_history WHERE id=1e2;", "SELECT * FROM play_history WHERE id=18446744073709551616;",
        "SELECT * FROM play_history WHERE id=1 AND song_id=2;", "SELECT * FROM play_history WHERE unknown=1;",
        "SELECT * FROM play_history SELECT * FROM play_history;", "SELECT * FROM play_history WHERE id=1 garbage;",
        "SELECT * FROM play_history WHERE event_code='unterminated", "SELECT * FROM play_history WHERE event_code='line\nbreak';",
        "SELECT * FROM play_history WHERE event_code='12345678901234567890123456789012';",
        "INSERT INTO play_history (event_code,user_id,song_id) VALUES ('P',1,2);",
        "INSERT INTO play_history (event_code,user_id,song_id,user_id) VALUES ('P',1,2,3);",
        "INSERT INTO play_history (event_code,user_id,song_id,id) VALUES ('P',1,2,3);",
        "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P','1',2,'rock');",
        "INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',1,2,'rock','extra');",
        "SELECT * FROM play_history /* unsupported */;"
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(*cases); ++i) bad(cases[i]);
    char embedded[] = {'S','E','L','E','C','T',0,'*'};
    sql_parser parser; sql_statement s; sql_error error;
    sql_parser_init(&parser, embedded, sizeof(embedded));
    assert(sql_next(&parser, &s, &error) == SQL_ERROR);
    sql_parser_init(&parser, NULL, 1);
    assert(sql_next(&parser, &s, &error) == SQL_ERROR);
    assert(sql_next(NULL, &s, &error) == SQL_ERROR);
}

static void test_error_position_and_boundaries(void) {
    const char *text = "-- first\nSELECT * FROM play_history WHERE id = -1;";
    sql_parser parser; sql_statement s; sql_error error;
    sql_parser_init(&parser, text, strlen(text));
    assert(sql_next(&parser, &s, &error) == SQL_ERROR && error.line == 2);
    char long_word[200]; memset(long_word, 'a', sizeof(long_word)); long_word[199] = 0;
    bad(long_word);
    for (unsigned i = 0; i < 10000; ++i) {
        s = parse("SELECT * FROM play_history WHERE id = 999;");
        assert(s.value.number == 999);
    }
}

static void test_deterministic_arbitrary_input(void) {
    uint32_t random = 1234567;
    for (unsigned test = 0; test < 10000; ++test) {
        char text[128];
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        size_t length = random % sizeof(text);
        for (size_t i = 0; i < length; ++i) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            text[i] = (char)(random >> 24);
        }
        sql_parser parser; sql_statement s; sql_error error;
        sql_parser_init(&parser, text, length);
        for (size_t attempt = 0; attempt <= length + 1; ++attempt) {
            size_t before = parser.position;
            sql_status status = sql_next(&parser, &s, &error);
            assert(parser.position <= length);
            if (status != SQL_OK) break;
            assert(parser.position > before);
            assert(s.kind == SQL_INSERT || s.kind == SQL_SELECT);
        }
    }
}

int main(void) {
    test_insert(); puts("PASS INSERT values, column ordering and case");
    test_select(); puts("PASS SELECT fields, typed values and quote escapes");
    test_scripts_and_owned_values(); puts("PASS scripts, comments and owned statement values");
    test_bad_syntax_and_types(); puts("PASS invalid and unsupported SQL rejection");
    test_error_position_and_boundaries(); puts("PASS error locations, boundaries and repeated parsing");
    test_deterministic_arbitrary_input(); puts("PASS 10000 arbitrary byte inputs and parser progress");
    return 0;
}
