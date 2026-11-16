#include "engine.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static unsigned syncs;
static int fail_sync;
int __real_fsync(int);
int __wrap_fsync(int fd) {
    syncs++;
    if (fail_sync) { fail_sync=0; errno=EIO; return -1; }
    return __real_fsync(fd);
}
int main(void) {
    char dir[]="/tmp/mini-dbms-durable-XXXXXX", path[256];
    assert(mkdtemp(dir)); snprintf(path,sizeof(path),"%s/history.db",dir);
    db_engine *e; assert(engine_open(&e,path)==DB_OK);
    const char *sql="INSERT INTO play_history (event_code,user_id,song_id,genre) VALUES ('P',71,108,'rock');";
    sql_parser p; sql_statement s; sql_error error;
    sql_parser_init(&p,sql,strlen(sql)); assert(sql_next(&p,&s,&error)==SQL_OK);
    db_result r; syncs=0;
    assert(engine_execute_sync(e,&s,NULL,NULL,&r)==DB_OK && syncs==1 && r.inserted.id==1);
    r.inserted.id=999; fail_sync=1;
    assert(engine_execute_sync(e,&s,NULL,NULL,&r)==DB_IO_ERROR && syncs==2 && r.inserted.id==999);
    assert(engine_execute_sync(e,&s,NULL,NULL,&r)==DB_IO_ERROR);
    assert(engine_close(e)==DB_IO_ERROR);
    /* A failed acknowledgement is uncertain, not a rollback: row 2 can remain. */
    assert(engine_open(&e,path)==DB_OK);
    sql="SELECT * FROM play_history WHERE id=2;";
    sql_parser_init(&p,sql,strlen(sql)); assert(sql_next(&p,&s,&error)==SQL_OK);
    assert(engine_execute(e,&s,NULL,NULL,&r)==DB_OK && r.affected==1);
    assert(engine_close(e)==DB_OK); unlink(path); rmdir(dir);
    puts("PASS durable execute syncs before success and preserves uncertain failure semantics");
}
