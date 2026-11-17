#include "db.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HEADER_LIMIT 8192
#define BODY_LIMIT 65536
#define OUTPUT_LIMIT (4 * 1024 * 1024)
#define REQUEST_SECONDS 5

typedef struct {
    int *fds;
    size_t capacity, head, count;
    int stopping;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    DbEngine *engine;
} Pool;

static volatile sig_atomic_t stopping;


static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + time.tv_nsec / 1e9;
}

static ssize_t receive(int fd, void *buffer, size_t length, double deadline) {
    for (;;) {
        int milliseconds = (int)((deadline - now()) * 1000);
        if (milliseconds <= 0) return -1;
        struct pollfd event = {.fd = fd, .events = POLLIN};
        int ready = poll(&event, 1, milliseconds);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t got = recv(fd, buffer, length, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return got;
    }
}

static int send_all(int fd, const char *data, size_t length) {
    double deadline = now() + REQUEST_SECONDS;
    while (length) {
        int milliseconds = (int)((deadline - now()) * 1000);
        if (milliseconds <= 0) return -1;
        struct pollfd event = {.fd = fd, .events = POLLOUT};
        int ready = poll(&event, 1, milliseconds);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return -1;
        ssize_t sent = send(fd, data, length, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return -1;
        data += sent; length -= (size_t)sent;
    }
    return 0;
}

static char *json_quote(const char *value) {
    size_t length = strlen(value);
    if (length > (SIZE_MAX - 3) / 6) return NULL;
    char *result = malloc(length * 6 + 3), *out = result;
    if (!result) return NULL;
    *out++ = '"';
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        if (*p == '"' || *p == '\\') { *out++ = '\\'; *out++ = (char)*p; }
        else if (*p < 32) { snprintf(out, 7, "\\u%04x", *p); out += 6; }
        else *out++ = (char)*p;
    }
    *out++ = '"'; *out = 0;
    return result;
}

static int valid_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        unsigned code = *p++;
        if (code < 0x80) continue;
        unsigned count, minimum;
        if (code >= 0xc2 && code <= 0xdf) { count = 1; minimum = 0x80; code &= 31; }
        else if (code >= 0xe0 && code <= 0xef) { count = 2; minimum = 0x800; code &= 15; }
        else if (code >= 0xf0 && code <= 0xf4) { count = 3; minimum = 0x10000; code &= 7; }
        else return 0;
        for (unsigned i = 0; i < count; ++i) {
            if ((*p & 0xc0) != 0x80) return 0;
            code = (code << 6) | (*p++ & 63);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return 0;
    }
    return 1;
}

static void respond(int fd, int status, const char *value) {
    if (!valid_utf8(value)) { status = 500; value = "Result is not valid UTF-8"; }
    const char *reason = status == 200 ? "OK" : status == 400 ? "Bad Request" :
                         status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed" :
                         status == 413 ? "Content Too Large" : status == 415 ? "Unsupported Media Type" :
                         status == 503 ? "Service Unavailable" : "Internal Server Error";
    char *quoted = json_quote(value);
    if (!quoted) return;
    size_t capacity = strlen(quoted) + 64;
    char *body = malloc(capacity);
    if (!body) { free(quoted); return; }
    snprintf(body, capacity, status == 200 ? "{\"ok\":true,\"output\":%s}" :
             "{\"ok\":false,\"error\":%s}", quoted);
    char header[256];
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
        status, reason, strlen(body), status == 405 ? "Allow: POST\r\n" : "");
    if (!send_all(fd, header, (size_t)length)) send_all(fd, body, strlen(body));
    free(body); free(quoted);
}

static void whitespace(const char **p) { while (strchr(" \t\r\n", **p) && **p) ++*p; }
static int hex4(const char **p, unsigned *value) {
    *value = 0;
    for (int i = 0; i < 4; ++i) {
        unsigned char c = (unsigned char)**p;
        if (!isxdigit(c)) return -1;
        *value = *value * 16 + (isdigit(c) ? c - '0' : (unsigned)tolower(c) - 'a' + 10);
        ++*p;
    }
    return 0;
}

static char *json_string(const char **p) {
    if (**p != '"') return NULL;
    ++*p;
    char *value = malloc(strlen(*p) + 1), *out = value;
    if (!value) return NULL;
    while (**p && **p != '"') {
        unsigned char c = (unsigned char)*(*p)++;
        if (c < 32) goto invalid;
        if (c != '\\') { *out++ = (char)c; continue; }
        c = (unsigned char)*(*p)++;
        if (!c) goto invalid;
        if (c == '"' || c == '\\' || c == '/') *out++ = (char)c;
        else if (c == 'b') *out++ = '\b';
        else if (c == 'f') *out++ = '\f';
        else if (c == 'n') *out++ = '\n';
        else if (c == 'r') *out++ = '\r';
        else if (c == 't') *out++ = '\t';
        else if (c == 'u') {
            unsigned code;
            if (hex4(p, &code) || !code) goto invalid;
            if (code >= 0xd800 && code <= 0xdbff) {
                if ((*p)[0] != '\\' || (*p)[1] != 'u') goto invalid;
                *p += 2;
                unsigned low;
                if (hex4(p, &low) || low < 0xdc00 || low > 0xdfff) goto invalid;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) goto invalid;
            if (code < 0x80) *out++ = (char)code;
            else if (code < 0x800) { *out++ = (char)(0xc0 | (code >> 6)); *out++ = (char)(0x80 | (code & 63)); }
            else if (code < 0x10000) { *out++ = (char)(0xe0 | (code >> 12)); *out++ = (char)(0x80 | ((code >> 6) & 63)); *out++ = (char)(0x80 | (code & 63)); }
            else { *out++ = (char)(0xf0 | (code >> 18)); *out++ = (char)(0x80 | ((code >> 12) & 63)); *out++ = (char)(0x80 | ((code >> 6) & 63)); *out++ = (char)(0x80 | (code & 63)); }
        } else goto invalid;
    }
    if (**p != '"') goto invalid;
    ++*p; *out = 0; return value;
invalid:
    free(value); return NULL;
}

static char *parse_sql(const char *body) {
    const char *p = body;
    whitespace(&p);
    if (*p++ != '{') return NULL;
    whitespace(&p);
    char *name = json_string(&p);
    if (!name) return NULL;
    int valid = !strcmp(name, "sql"); free(name);
    whitespace(&p);
    if (!valid || *p++ != ':') return NULL;
    whitespace(&p);
    char *sql = json_string(&p);
    if (!sql) return NULL;
    whitespace(&p);
    if (*p != '}') { free(sql); return NULL; }
    ++p; whitespace(&p);
    if (*p) { free(sql); return NULL; }
    return sql;
}

static void handle(int fd, DbEngine *engine) {
    char header[HEADER_LIMIT + 1];
    size_t used = 0, header_bytes = 0;
    double deadline = now() + REQUEST_SECONDS;
    /* Read until the header terminator; body bytes already read are retained. */
    while (used < HEADER_LIMIT) {
        ssize_t got = receive(fd, header + used, HEADER_LIMIT - used, deadline);
        if (got <= 0) { respond(fd, 400, "Incomplete or timed out request"); return; }
        used += (size_t)got; header[used] = 0;
        if (memchr(header, 0, used)) { respond(fd, 400, "NUL byte in request"); return; }
        char *end = strstr(header, "\r\n\r\n");
        if (end) { header_bytes = (size_t)(end - header) + 4; break; }
    }
    if (!header_bytes) { respond(fd, 413, "Headers too large"); return; }
    size_t prefix = used - header_bytes;
    char *body = malloc(BODY_LIMIT + 1);
    if (!body) { respond(fd, 500, "Out of memory"); return; }
    memcpy(body, header + header_bytes, prefix);
    header[header_bytes - 2] = 0;
    char *line_end = strstr(header, "\r\n");
    if (!line_end) { free(body); respond(fd, 400, "Invalid request line"); return; }
    *line_end = 0;
    char method[16], target[256], version[16], extra;
    if (sscanf(header, "%15s %255s %15s %c", method, target, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.1") && strcmp(version, "HTTP/1.0"))) {
        free(body); respond(fd, 400, "Invalid request line"); return;
    }
    size_t length = 0;
    int have_length = 0, have_type = 0;
    for (char *line = line_end + 2; *line;) {
        char *end = strstr(line, "\r\n");
        if (!end) { free(body); respond(fd, 400, "Invalid headers"); return; }
        *end = 0;
        char *colon = strchr(line, ':');
        if (!colon || colon == line) { free(body); respond(fd, 400, "Invalid header"); return; }
        *colon++ = 0;
        for (const unsigned char *name = (const unsigned char *)line; *name; ++name) {
            if (!isalnum(*name) && !strchr("!#$%&'*+-.^_`|~", *name)) {
                free(body); respond(fd, 400, "Invalid header name"); return;
            }
        }
        for (const unsigned char *value = (const unsigned char *)colon; *value; ++value) {
            if ((*value < 32 && *value != '\t') || *value == 127) {
                free(body); respond(fd, 400, "Invalid header value"); return;
            }
        }
        while (*colon == ' ' || *colon == '\t') ++colon;
        char *tail = colon + strlen(colon);
        while (tail > colon && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(line, "Transfer-Encoding")) { free(body); respond(fd, 400, "Transfer-Encoding is unsupported"); return; }
        if (!strcasecmp(line, "Content-Length")) {
            if (have_length++ || !*colon) { free(body); respond(fd, 400, "Invalid Content-Length"); return; }
            for (const char *digit = colon; *digit; ++digit) {
                if (!isdigit((unsigned char)*digit)) { free(body); respond(fd, 400, "Invalid Content-Length"); return; }
                if (length > BODY_LIMIT / 10 || length * 10 + (size_t)(*digit - '0') > BODY_LIMIT) {
                    free(body); respond(fd, 413, "Body too large"); return;
                }
                length = length * 10 + (size_t)(*digit - '0');
            }
        } else if (!strcasecmp(line, "Content-Type")) {
            if (have_type++) { free(body); respond(fd, 400, "Duplicate Content-Type"); return; }
            if (strcasecmp(colon, "application/json") && strcasecmp(colon, "application/json; charset=utf-8")) {
                free(body); respond(fd, 415, "Expected application/json"); return;
            }
        }
        line = end + 2;
    }
    if (strcmp(method, "POST")) { free(body); respond(fd, 405, "Use POST"); return; }
    if (strcmp(target, "/query")) { free(body); respond(fd, 404, "Unknown path"); return; }
    if (!have_length || !have_type || prefix > length) { free(body); respond(fd, 400, "Expected Content-Length and Content-Type"); return; }
    while (prefix < length) {
        ssize_t got = receive(fd, body + prefix, length - prefix, deadline);
        if (got <= 0) { free(body); respond(fd, 400, "Incomplete or timed out body"); return; }
        prefix += (size_t)got;
    }
    if (memchr(body, 0, length)) { free(body); respond(fd, 400, "NUL byte in body"); return; }
    body[length] = 0;
    char *sql = valid_utf8(body) ? parse_sql(body) : NULL; free(body);
    if (!sql) { respond(fd, 400, "Expected a JSON object with one sql string"); return; }
    FILE *output = tmpfile();
    if (!output) { free(sql); respond(fd, 500, "Cannot create response buffer"); return; }
    char error[512];
    int result = db_engine_execute(engine, sql, output, error, sizeof(error));
    free(sql);
    if (result) {
        fclose(output);
        int status = strstr(error, "Engine requires") || strstr(error, "roll back") ? 503 :
                     strstr(error, "output limit") ? 413 :
                     strstr(error, "Cannot") || strstr(error, "Out of memory") || strstr(error, "Corrupt") ? 500 : 400;
        respond(fd, status, error); return;
    }
    long output_size = ftell(output);
    if (output_size < 0 || output_size > OUTPUT_LIMIT || fseek(output, 0, SEEK_SET)) {
        fclose(output); respond(fd, 413, "Query result too large"); return;
    }
    char *text = malloc((size_t)output_size + 1);
    if (!text) { fclose(output); respond(fd, 500, "Out of memory"); return; }
    if (fread(text, 1, (size_t)output_size, output) != (size_t)output_size) {
        free(text); fclose(output); respond(fd, 500, "Cannot read response buffer"); return;
    }
    text[output_size] = 0;
    fclose(output); respond(fd, 200, text); free(text);
}

static void *worker(void *argument) {
    Pool *pool = argument;
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->count && !pool->stopping) pthread_cond_wait(&pool->ready, &pool->mutex);
        if (pool->stopping) { pthread_mutex_unlock(&pool->mutex); return NULL; }
        int fd = pool->fds[pool->head];
        pool->head = (pool->head + 1) % pool->capacity;
        --pool->count;
        pthread_mutex_unlock(&pool->mutex);
        handle(fd, pool->engine); close(fd);
    }
}

static int number(const char *text, long maximum, long *value) {
    char *end;
    errno = 0;
    *value = strtol(text, &end, 10);
    return !*text || *end || errno || *value < 0 || *value > maximum;
}

int main(int argc, char **argv) {
    long port = 8080, threads = 4, capacity = 64;
    if (argc < 2 || argc > 5 || (argc > 2 && number(argv[2], 65535, &port)) ||
        (argc > 3 && number(argv[3], 128, &threads)) || !threads ||
        (argc > 4 && number(argv[4], 4096, &capacity)) || !capacity) {
        fprintf(stderr, "Usage: %s DATABASE_DIRECTORY [PORT=8080] [WORKERS=4] [QUEUE=64]\n", argv[0]); return 1;
    }
    char error[512];
    DbEngine *engine = db_engine_open(argv[1], error, sizeof(error));
    if (!engine) { fprintf(stderr, "%s\n", error); return 1; }
    signal(SIGPIPE, SIG_IGN);
    sigset_t signals;
    sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
    /* Workers inherit blocked stop signals; the listener observes pending signals. */
    if (pthread_sigmask(SIG_BLOCK, &signals, NULL)) { db_engine_close(engine); return 1; }
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int reuse = 1;
    if (listener >= 0) setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    address.sin_addr.s_addr = htonl(0x7f000001UL);
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, (int)capacity)) {
        perror("listen"); if (listener >= 0) close(listener); db_engine_close(engine); return 1;
    }
    if (fcntl(listener, F_SETFL, O_NONBLOCK) < 0) { close(listener); db_engine_close(engine); return 1; }
    Pool pool = {.capacity = (size_t)capacity, .engine = engine};
    pool.fds = calloc(pool.capacity, sizeof(*pool.fds));
    pthread_t *workers = calloc((size_t)threads, sizeof(*workers));
    if (!pool.fds || !workers) { free(pool.fds); free(workers); close(listener); db_engine_close(engine); return 1; }
    if (pthread_mutex_init(&pool.mutex, NULL)) {
        free(pool.fds); free(workers); close(listener); db_engine_close(engine); return 1;
    }
    if (pthread_cond_init(&pool.ready, NULL)) {
        pthread_mutex_destroy(&pool.mutex);
        free(pool.fds); free(workers); close(listener); db_engine_close(engine); return 1;
    }
    long created = 0;
    while (created < threads && !pthread_create(&workers[created], NULL, worker, &pool)) ++created;
    if (created != threads) stopping = 1;
    socklen_t address_size = sizeof(address);
    getsockname(listener, (struct sockaddr *)&address, &address_size);
    if (!stopping) { printf("Listening on 127.0.0.1:%u\n", ntohs(address.sin_port)); fflush(stdout); }
    while (!stopping) {
        sigset_t pending;
        sigpending(&pending);
        if (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)) break;
        struct pollfd event = {.fd = listener, .events = POLLIN};
        int ready = poll(&event, 1, 250);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;
        if (!ready) continue;
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) continue;
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0) { close(fd); continue; }
        pthread_mutex_lock(&pool.mutex);
        if (pool.count == pool.capacity) { pthread_mutex_unlock(&pool.mutex); respond(fd, 503, "Request queue is full"); close(fd); }
        else {
            pool.fds[(pool.head + pool.count) % pool.capacity] = fd; ++pool.count;
            pthread_cond_signal(&pool.ready); pthread_mutex_unlock(&pool.mutex);
        }
    }
    close(listener);
    pthread_mutex_lock(&pool.mutex);
    pool.stopping = 1;
    while (pool.count) { close(pool.fds[pool.head]); pool.head = (pool.head + 1) % pool.capacity; --pool.count; }
    pthread_cond_broadcast(&pool.ready); pthread_mutex_unlock(&pool.mutex);
    for (long i = 0; i < created; ++i) pthread_join(workers[i], NULL);
    pthread_cond_destroy(&pool.ready); pthread_mutex_destroy(&pool.mutex);
    free(pool.fds); free(workers); db_engine_close(engine);
    return created == threads ? 0 : 1;
}
