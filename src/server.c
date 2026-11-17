#include "engine.h"
#include "pool.h"
#include "picohttpparser.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HEADER_LIMIT 8192
#define SQL_LIMIT 16384
#define ROW_LIMIT 1000
#define RESPONSE_LIMIT (256*1024)
typedef struct { char *data; size_t length, capacity, rows; int full; } buffer;
typedef struct { db_engine *engine; worker_pool *pool; unsigned workers; } server;
static volatile sig_atomic_t stopping;
static void stop_signal(int signal_number) { (void)signal_number; stopping = 1; }
static int64_t milliseconds(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) return 0;
    return (int64_t)t.tv_sec*1000 + t.tv_nsec/1000000;
}
static int wait_io(int fd, short events, int64_t deadline) {
    for (;;) {
        int64_t left = deadline - milliseconds();
        if (left <= 0) return -1;
        struct pollfd p = {fd, events, 0};
        int ready = poll(&p, 1, (int)left);
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 && (p.revents & events) ? 0 : -1;
    }
}
static int send_bytes(int fd, const char *data, size_t length, int64_t deadline) {
    while (length) {
        ssize_t sent = send(fd, data, length, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!wait_io(fd, POLLOUT, deadline)) continue;
        }
        if (sent <= 0) return -1;
        data += sent; length -= (size_t)sent;
    }
    return 0;
}
static void respond(int fd, int code, const char *body) {
    char header[256]; size_t length = strlen(body);
    int size = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                        code, code == 200 ? "OK" : "Error", length);
    int64_t deadline = milliseconds()+5000;
    if (size > 0 && !send_bytes(fd, header, (size_t)size, deadline)) send_bytes(fd, body, length, deadline);
}
static void append(buffer *b, const char *format, ...) {
    if (b->full) return;
    va_list args; va_start(args, format);
    int n = vsnprintf(b->data+b->length, b->capacity-b->length, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= b->capacity-b->length) { b->full=1; return; }
    b->length += (size_t)n;
}
static void json_string(buffer *b, const char *text) {
    append(b, "\"");
    for (; *text; ++text) {
        unsigned char c=(unsigned char)*text;
        if (c == '"' || c == '\\') append(b, "\\%c", c);
        else if (c < 32) append(b, "\\u%04x", c);
        else append(b, "%c", c);
    }
    append(b, "\"");
}
static void error_response(int fd, int code, const char *message) {
    char text[1024]; buffer b={.data=text,.capacity=sizeof(text)};
    append(&b,"{\"status\":\"error\",\"message\":"); json_string(&b,message); append(&b,"}");
    respond(fd,code,b.full ? "{\"status\":\"error\"}" : text);
}
static void encode_row(buffer *b, const play_record *r) {
    append(b,"{\"id\":%" PRIu64 ",\"event_code\":",r->id); json_string(b,r->event_code);
    append(b,",\"user_id\":%" PRIu64 ",\"song_id\":%" PRIu64 ",\"genre\":",r->user_id,r->song_id);
    json_string(b,r->genre); append(b,"}");
}
static int collect_row(const play_record *r, void *context) {
    buffer *b=context;
    if (b->rows == ROW_LIMIT) { b->full=1; return -1; }
    if (b->rows++) append(b,",");
    encode_row(b,r); return b->full ? -1 : 0;
}
static int equal(const char *text, size_t length, const char *expected) {
    return length == strlen(expected) && !memcmp(text,expected,length);
}
static int header_is(const struct phr_header *h, const char *name) {
    return h->name && h->name_len == strlen(name) && !strncasecmp(h->name,name,h->name_len);
}
static int decimal(const char *text, size_t length, size_t *out) {
    if (!length) return -1;
    size_t value=0;
    for (size_t i=0;i<length;++i) {
        if (text[i]<'0' || text[i]>'9' || value > (SIZE_MAX-(size_t)(text[i]-'0'))/10) return -1;
        value=value*10+(size_t)(text[i]-'0');
    }
    *out=value; return 0;
}
static void handle(int fd, unsigned worker_id, void *context) {
    server *s=context;
    char input[HEADER_LIMIT+SQL_LIMIT+1]; size_t used=0;
    const char *method=NULL,*path=NULL; size_t method_length=0,path_length=0,num_headers;
    struct phr_header headers[64]; int minor, header_size;
    int64_t deadline=milliseconds()+5000;
    for (;;) {
        if (wait_io(fd,POLLIN,deadline)) { error_response(fd,408,"request timeout"); return; }
        ssize_t n=recv(fd,input+used,sizeof(input)-1-used,MSG_DONTWAIT);
        if (n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) return;
        used+=(size_t)n; num_headers=64;
        header_size=phr_parse_request(input,used,&method,&method_length,&path,&path_length,&minor,headers,&num_headers,0);
        if (header_size>0) break;
        if (used>=HEADER_LIMIT) { error_response(fd,413,"HTTP headers too large"); return; }
        if (header_size == -1) { error_response(fd,400,"invalid HTTP request"); return; }
    }
    if (header_size>HEADER_LIMIT) { error_response(fd,413,"HTTP headers too large"); return; }
    size_t body_length=0; int lengths=0, hosts=0, media=0, types=0;
    for (size_t i=0;i<num_headers;++i) {
        struct phr_header *h=&headers[i];
        if (!h->name) { error_response(fd,400,"folded headers unsupported"); return; }
        if (header_is(h,"transfer-encoding")) { error_response(fd,400,"Transfer-Encoding unsupported; use Content-Length"); return; }
        if (header_is(h,"expect")) { error_response(fd,417,"Expect unsupported"); return; }
        if (header_is(h,"host")) { if (!h->value_len || ++hosts>1) { error_response(fd,400,"invalid Host"); return; } }
        if (header_is(h,"content-length")) {
            if (++lengths>1 || decimal(h->value,h->value_len,&body_length)) { error_response(fd,400,"invalid Content-Length"); return; }
        }
        if (header_is(h,"content-type")) {
            if (++types>1) { error_response(fd,400,"duplicate Content-Type"); return; }
            const char *allowed[]={"text/plain","application/sql"};
            for (unsigned j=0;j<2;++j) {
                size_t len=strlen(allowed[j]);
                if (h->value_len>=len && !strncasecmp(h->value,allowed[j],len) &&
                    (h->value_len==len || h->value[len]==';')) media=1;
            }
        }
    }
    if (minor==1 && !hosts) { error_response(fd,400,"Host required"); return; }
    if (body_length>SQL_LIMIT) { error_response(fd,413,"SQL body exceeds 16384 bytes"); return; }
    int health=equal(path,path_length,"/health"), sql_route=equal(path,path_length,"/sql");
    if (!health && !sql_route) { error_response(fd,404,"route not found"); return; }
    if ((health && !equal(method,method_length,"GET")) || (sql_route && !equal(method,method_length,"POST"))) {
        error_response(fd,405,"method not allowed"); return;
    }
    if (health) {
        if (body_length || used>(size_t)header_size) { error_response(fd,400,"unexpected request body"); return; }
        char body[256]; unsigned active; size_t queued;
        pool_snapshot(s->pool,&active,&queued);
        snprintf(body,sizeof(body),"{\"status\":\"ok\",\"workers\":%u,\"active\":%u,\"queued\":%zu,\"worker_id\":%u}",s->workers,active,queued,worker_id);
        respond(fd,200,body); return;
    }
    if (!lengths) { error_response(fd,411,"Content-Length required"); return; }
    if (!media) { error_response(fd,415,"use Content-Type: text/plain or application/sql"); return; }
    size_t needed=(size_t)header_size+body_length;
    if (used>needed) { error_response(fd,400,"extra bytes after request body"); return; }
    while (used<needed) {
        if (wait_io(fd,POLLIN,deadline)) { error_response(fd,408,"request timeout"); return; }
        ssize_t n=recv(fd,input+used,sizeof(input)-1-used,MSG_DONTWAIT);
        if (n<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (n<=0) return;
        used+=(size_t)n;
        if (used>needed) { error_response(fd,400,"extra bytes after request body"); return; }
    }
    char trailing;
    if (recv(fd,&trailing,1,MSG_PEEK|MSG_DONTWAIT)>0) { error_response(fd,400,"extra bytes after request body"); return; }
    sql_parser parser; sql_statement statement, extra; sql_error error;
    sql_parser_init(&parser,input+header_size,body_length);
    sql_status parsed=sql_next(&parser,&statement,&error);
    if (parsed!=SQL_OK) { error_response(fd,400,parsed==SQL_DONE ? "one SQL statement required" : error.message); return; }
    if (sql_next(&parser,&extra,&error)!=SQL_DONE) { error_response(fd,400,"exactly one valid SQL statement required"); return; }
    buffer b={.capacity=RESPONSE_LIMIT}; b.data=malloc(b.capacity);
    if (!b.data) { error_response(fd,500,"out of memory"); return; }
    append(&b,"{\"status\":\"ok\",\"worker_id\":%u,\"operation\":\"%s\",\"rows\":[",worker_id,statement.kind==SQL_INSERT ? "insert" : "select");
    db_result result;
    db_status status=engine_execute_sync(s->engine,&statement,collect_row,&b,&result);
    if (status!=DB_OK) {
        error_response(fd,b.full ? 413 : status==DB_INVALID_ARGUMENT ? 422 : 500,
                       b.full ? "result exceeds 1000 rows or response limit; use a narrower WHERE condition" : db_status_text(status));
        free(b.data); return;
    }
    append(&b,"],\"matched\":%" PRIu64 ",\"scanned\":%" PRIu64 ",\"access_path\":\"%s\",\"index_nodes\":%" PRIu64,
           statement.kind==SQL_SELECT ? result.affected : 0,result.scanned,result.used_index ? "bplus_tree" : "full_scan",result.index_nodes);
    if (statement.kind==SQL_INSERT) { append(&b,",\"inserted\":1,\"record\":"); encode_row(&b,&result.inserted); }
    append(&b,"}");
    if (b.full) error_response(fd,500,"response limit exceeded"); else respond(fd,200,b.data);
    free(b.data);
}
static int number(const char *text, unsigned maximum, unsigned *out) {
    size_t value;
    if (decimal(text,strlen(text),&value) || value>maximum) return -1;
    *out=(unsigned)value; return 0;
}
int main(int argc, char **argv) {
    const char *database="data/play_history_v2.db", *host="127.0.0.1";
    unsigned port=8091,workers=4,capacity=64; int custom=0;
    for (int i=1;i<argc;i+=2) {
        if (i+1==argc) { fputs("options require values\n",stderr); return 2; }
        if (!strcmp(argv[i],"--db")) { database=argv[i+1]; custom=1; }
        else if (!strcmp(argv[i],"--host")) host=argv[i+1];
        else if (!strcmp(argv[i],"--port")) { if (number(argv[i+1],65535,&port)) return 2; }
        else if (!strcmp(argv[i],"--workers")) { if (number(argv[i+1],32,&workers) || !workers) return 2; }
        else if (!strcmp(argv[i],"--queue")) { if (number(argv[i+1],1024,&capacity) || !capacity) return 2; }
        else { fprintf(stderr,"unknown option: %s\n",argv[i]); return 2; }
    }
    if (!custom && mkdir("data",0700)<0 && errno!=EEXIST) { perror("data"); return 1; }
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if (inet_pton(AF_INET,host,&address.sin_addr)!=1) { fputs("host must be IPv4\n",stderr); return 2; }
    struct sigaction action={0}; action.sa_handler=stop_signal; sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL); sigaction(SIGTERM,&action,NULL);
    int listener=socket(AF_INET,SOCK_STREAM,0), reuse=1;
    if (listener<0) { perror("socket"); return 1; }
    setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    if (fcntl(listener,F_SETFL,O_NONBLOCK)<0 || bind(listener,(struct sockaddr *)&address,sizeof(address)) || listen(listener,128)) {
        perror("listen"); close(listener); return 1;
    }
    server s={.workers=workers};
    db_status status=engine_open(&s.engine,database);
    if (status!=DB_OK) { fprintf(stderr,"%s\n",db_status_text(status)); close(listener); return 1; }
    /* Workers inherit blocked termination signals; only main runs stop_signal. */
    sigset_t term_signals, old_signals;
    sigemptyset(&term_signals); sigaddset(&term_signals,SIGINT); sigaddset(&term_signals,SIGTERM);
    if (pthread_sigmask(SIG_BLOCK,&term_signals,&old_signals)) { engine_close(s.engine); close(listener); return 1; }
    int pool_status=pool_create(&s.pool,workers,capacity,handle,&s);
    pthread_sigmask(SIG_SETMASK,&old_signals,NULL);
    if (pool_status) { engine_close(s.engine); close(listener); return 1; }
    socklen_t address_size=sizeof(address); getsockname(listener,(struct sockaddr *)&address,&address_size);
    printf("LISTENING %s:%u workers=%u queue=%u\n",host,ntohs(address.sin_port),workers,capacity); fflush(stdout);
    int failure=0;
    while (!stopping) {
        struct pollfd p={listener,POLLIN,0}; int ready=poll(&p,1,100);
        if (ready<0 && errno==EINTR) continue;
        if (ready<0) { failure=1; break; }
        if (!ready) continue;
        int fd=accept(listener,NULL,NULL);
        if (fd<0 && (errno==EINTR || errno==EAGAIN || errno==EWOULDBLOCK)) continue;
        if (fd<0) { failure=1; break; }
        if (pool_submit(s.pool,fd)) {
            static const char reply[]="HTTP/1.1 503 Busy\r\nContent-Length: 17\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"busy\"}";
            send(fd,reply,sizeof(reply)-1,MSG_NOSIGNAL|MSG_DONTWAIT); close(fd);
        }
    }
    close(listener); pool_destroy(s.pool);
    status=engine_close(s.engine);
    if (status!=DB_OK) fprintf(stderr,"shutdown: %s\n",db_status_text(status));
    return failure || status!=DB_OK;
}
