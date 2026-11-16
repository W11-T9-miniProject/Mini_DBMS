#include "internal.h"

DbStatus db_error_set(DbError *error, DbStatus code, const char *format, ...) {
    if (error) {
        va_list args;
        error->code = code;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return code;
}

const char *db_status_name(DbStatus status) {
    switch (status) {
        case DB_OK: return "OK";
        case DB_SQL_ERROR: return "SQL_ERROR";
        case DB_IO_ERROR: return "IO_ERROR";
        case DB_MEMORY_ERROR: return "MEMORY_ERROR";
        case DB_CORRUPT: return "CORRUPT";
        case DB_BUSY: return "BUSY";
        case DB_CALLBACK_ERROR: return "CALLBACK_ERROR";
    }
    return "UNKNOWN";
}

const char *db_plan_name(DbPlan plan) {
    switch (plan) {
        case DB_PLAN_INSERT: return "INSERT";
        case DB_PLAN_SCAN: return "TABLE SCAN";
        case DB_PLAN_INDEX_POINT: return "INDEX LOOKUP";
        case DB_PLAN_INDEX_RANGE: return "INDEX RANGE SCAN";
    }
    return "UNKNOWN";
}
