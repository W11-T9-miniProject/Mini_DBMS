# Mini DBMS — Phase 3

A small C11 database with CLI and HTTP interfaces sharing a SQL engine and file storage.
Existing tables support single-row `INSERT` and streaming `SELECT`.

```sh
make
./mini_dbms DATABASE_DIRECTORY SQL_FILE
make test
```

Requires a C11 compiler, Make, POSIX threads/sockets and `getline`/`strdup`; tests require Python 3.
The CLI reads the entire SQL file and executes statements in order. Successful
execution exits 0; failures exit 1 and write a diagnostic to stderr. INSERT has
no stdout output. SELECT prints rows in insertion order, with no header or
status messages. Each result row ends with a newline. Fields are comma-separated;
fields containing commas or double quotes are CSV-quoted, with double quotes
doubled. Multiple SELECT results are concatenated.

## Existing tables

Create the database directory and a `<table>.schema` file before running SQL.
For example, `people.schema`:

```text
id INTEGER
name TEXT
age INTEGER
```

Each nonblank schema line contains a unique column name and exactly one type:
`INTEGER` or `TEXT` (uppercase). Schema order determines `INSERT` without a column
list and `SELECT *` order. Names match `[A-Za-z_][A-Za-z0-9_]*` and are
case-sensitive. SQL keywords are case-insensitive. Use ASCII identifiers.
An absent `.rows` file represents an empty table and is created by INSERT.
Do not change the schema after adding data.

## SQL

```sql
INSERT INTO people VALUES (1, 'Ada', 36);
INSERT INTO people (name, age, id) VALUES ('O''Brien', 31, 2);
SELECT * FROM people;
SELECT name, id FROM people WHERE age >= 30 AND (name != 'Bob' OR id = 1);
```

Supported grammar:

```text
INSERT INTO table [(column, ...)] VALUES (literal, ...);
SELECT (* | column, ...) FROM table [WHERE expression];
expression := expression OR expression
            | expression AND expression
            | (expression)
            | column (= | != | < | <= | > | >=) literal
```

AND binds more tightly than OR; parentheses override precedence. Every statement
requires a semicolon. Explicit INSERT column lists must include every schema
column exactly once, in any order. A table with an exact `id INTEGER` column may
omit only `id` from a column list; the engine assigns the next positive ID.
Positional `INSERT INTO table VALUES (...)` still requires every column. SELECT projections may reorder or repeat
columns. Predicates are validated even when a table is empty.

INTEGER literals are signed 64-bit decimal integers, optionally prefixed with
`+` or `-`. Values are stored/printed in canonical decimal form. TEXT literals
use single quotes; `''` escapes a single quote. Backslashes are ordinary text.
Literal types must match column types, with no implicit conversion. INTEGER
comparisons are numeric; TEXT comparisons use bytewise, case-sensitive order.
TEXT supports empty strings, commas, and double quotes, but rejects CR/LF so
one result row always occupies one output line. NUL bytes are rejected.

## Modules

- `src/main.c`: reads the SQL file, invokes the engine, reports errors.
- `src/engine.c`: lexer, parser, validation, predicate evaluation, CSV output.
  Its interface is `db_execute(directory, sql, output, error, error_size)`.
- `src/storage.c`: loads schemas, appends rows, and streams/validates stored rows.
  Storage declarations and ownership rules are in `src/db.h`.
- `src/bptree.c`: memory-resident B+ tree mapping an ID to its byte offset in a
  row file. Its public interface is in `src/bptree.h`.

Tests exercise the CLI interface with temporary database directories, including
persistence across separate processes. Implementation followed failing CLI
examples for persistence, column mapping/projection, WHERE, and CSV/error cases.

## Storage and limits

`<table>.rows` is binary, without a header. In schema order, each field is a
4-byte unsigned little-endian byte length followed by that many bytes (no NUL
terminator). Rows concatenate directly. INTEGER fields contain canonical ASCII
decimal text; TEXT fields contain literal bytes. Truncated fields, embedded NUL,
invalid integer payloads, and oversized fields are reported as corrupt data.

Limits: 128 columns, 128 SELECT projection entries, 1 MiB per TEXT value,
256 WHERE expression nodes (comparisons and boolean operators), and 64 nested
parenthesis levels. Identifiers in schema files are at most 255 bytes.
The SQL file is held in memory; SELECT streams one row at a time.

Phase 2 builds a memory-resident B+ tree for each table with an exact `id INTEGER`
column. It is rebuilt from `<table>.rows` on first ID-index use in each CLI
invocation, then retained while that SQL file runs. A root `WHERE id = integer`
predicate uses the tree to seek directly to its row. Other predicates scan the
full table. The index is not persisted, and duplicate IDs are rejected.

Run `make bench` to create a temporary one-million-row database and report the
insert duration, index rebuild plus first ID lookup, warmed ID lookup estimate,
and linear `age` lookup time. The benchmark deletes its generated data.

The project omits CREATE/DROP, UPDATE/DELETE, NULL/defaults, constraints, joins,
aggregates, ordering, comments, quoted identifiers, expressions as values,
multi-row VALUES, transactions, and crash recovery. A cooperative database-directory
lock excludes concurrent CLI/server processes. A file runs until its first error; previous statements remain
applied, and previously printed rows remain output. Individual INSERT syntax
and types are checked before appending. A failed append or failed index allocation attempts to truncate the new row.
The server requires restart after a memory/write failure and refuses more queries
if state may need rebuilding. There is no crash recovery or fsync durability guarantee. SELECT performs a full table scan except for a root
`id = integer` predicate.

Try the example against a disposable copy to avoid appending duplicate rows:

```sh
cp -R examples/db /tmp/mini-db-demo
./mini_dbms /tmp/mini-db-demo examples/demo.sql
```


## HTTP server (Phase 3)

Create schemas before starting the server. For a disposable demonstration:

```sh
make
cp -R examples/db /tmp/mini-db-api
./mini_dbms_server /tmp/mini-db-api 8080 4 64
```

Arguments: database directory, optional port (default 8080, `0` chooses an available
port), worker count (default 4, maximum 128), waiting queue size (default 64,
maximum 4096). The startup line prints the actual port. It binds only to
`127.0.0.1`. HTTP is local and has no TLS/authentication. A separate client process
can run these requests:

```sh
curl -i http://127.0.0.1:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"INSERT INTO people (name, age) VALUES ('"'"'Mina'"'"', 24);"}'
curl -i http://127.0.0.1:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT name FROM people WHERE id = 1;"}'
```

`POST /query` accepts exactly one JSON property, `sql`, with exactly one terminated
SQL statement. INSERT success returns `{"ok":true,"output":""}`. SELECT success
returns CSV text in `output`, e.g. `{"ok":true,"output":"Mina\n"}`. Errors return
`{"ok":false,"error":"..."}`. Results are isolated per request, without mixed
stdout across workers. SQL batches remain available through the CLI.

Status codes: 200 success; 400 malformed HTTP/JSON/SQL or unsupported batches;
404 unknown path; 405 unsupported method; 413 request/result too large;
415 unsupported media type; 500 engine/I/O failures; 503 waiting queue full or
engine restart required. Connections close after one response. Requests require
Content-Length and application/json; transfer encoding, chunking, keep-alive, and
pipelining are unsupported. JSON uses UTF-8 and supports Unicode escapes, including
surrogate pairs. NUL and malformed UTF-8 are rejected.

The listener enqueues connected sockets in a bounded queue. Fixed worker threads
read and execute requests. A slow client occupies one worker, with a five-second
absolute receive deadline once a worker begins reading, and bounded response send
time. Header size is at most 8 KiB; JSON body size at most 64 KiB; query CSV output
at most 4 MiB (the JSON encoding may be larger). Queue overflow receives 503.
SIGINT/Ctrl+C or SIGTERM closes the listener, discards waiting sockets, lets active
workers finish their bounded work, joins workers, then frees the engine. A query
already executing finishes before shutdown; there is no SQL execution cancellation.

`DbEngine` owns indexes for its lifetime. Startup validates schemas and reconstructs
ID indexes from existing row files before accepting connections. Each request has
its own parser, output buffer, and FILE handles. SELECT holds a shared read lock;
multiple SELECTs can traverse the same stable index concurrently. INSERT holds an
exclusive write lock across ID generation, duplicate detection, row append, index
registration, and next-ID update. Writers serialize globally across all tables.
A failed index insertion truncates its new row before reporting failure. If a
memory/storage failure leaves uncertain state, queries fail closed until restart.

The `.engine.lock` file provides a POSIX advisory lock for one engine process per
database directory. Both CLI and server honor it. External tools must not edit
`.schema` or `.rows` while a server is running. New tables/schema changes require a
restart. Use only local filesystems with working POSIX locks. Crashes and power loss
remain outside the durability guarantee; this is not a transactional DBMS.

`make test` runs the public B+tree tests, existing CLI tests, and real TCP HTTP
client tests: auto IDs, persistence/rebuild after restart, concurrent inserts and
mixed reads/writes, duplicate-free IDs, malformed requests, partial reads, Unicode,
queue overload, process exclusion, and shutdown with slow clients.
