#ifndef MINI_DB_SCRIPT_H
#define MINI_DB_SCRIPT_H

#include "db.h"
#include <stdio.h>

typedef DbStatus (*DbStatementVisitor)(const char *sql, void *context, DbError *error);

/* Streams statements from a SQL file. A semicolon inside a string or comment
 * is not a separator. Stops at the first error; previous statements remain. */
DbStatus db_run_script(FILE *input, DbStatementVisitor visitor, void *context, DbError *error);

#endif
