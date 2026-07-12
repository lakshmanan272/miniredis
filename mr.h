/* Mini-Redis (C) - a small Redis clone: RESP protocol over TCP + an HTTPS gateway.
 * Master header: shared types + all cross-file prototypes. */
#ifndef MR_H
#define MR_H

#include <stddef.h>

/* ---------------- hash table (dict.c) ---------------- */
typedef struct dictEntry {
    char *key;
    char *val;
    long long expire_at_ms;      /* 0 = never expires */
    struct dictEntry *next;
} dictEntry;

typedef struct {
    dictEntry **buckets;
    size_t nbuckets;
    size_t count;
} dict;

dict *dict_new(size_t nbuckets);
dictEntry *dict_find(dict *d, const char *key);                 /* NULL if absent (no expiry check) */
void       dict_set(dict *d, const char *key, const char *val, long long expire_at_ms);
int        dict_del(dict *d, const char *key);                  /* 1 if it existed */
void       dict_clear(dict *d);

/* ---------------- thread-safe store (store.c) ---------------- */
typedef struct {
    long long uptime_s, keys, commands, hits, misses, expired;
} Stats;

void       store_init(void);
int        store_set(const char *key, const char *val, long long ttl_ms, int nx); /* 1 if written */
char      *store_get(const char *key);                          /* malloc'd copy or NULL (caller frees) */
int        store_del(const char *key);                          /* 1 if existed */
int        store_del_if_value(const char *key, const char *want); /* safe unlock */
int        store_exists(const char *key);
int        store_expire(const char *key, long long ttl_ms);     /* 1 if key existed */
long long  store_ttl(const char *key);                          /* -2 missing, -1 no-expire, else seconds */
int        store_incrby(const char *key, long long delta, long long *out); /* 0 if not an int */
char     **store_keys(size_t *n);                               /* malloc'd array of malloc'd strings */
void       store_free_keys(char **keys, size_t n);
void       store_flush(void);
void       store_count_command(void);
Stats      store_stats(void);
void       store_sweep(void);                                   /* drop expired keys */

/* ---------------- util (util.c) ---------------- */
long long now_ms(void);
int  write_all(int fd, const char *buf, size_t len);
void gen_token(char out[25]);                                   /* 24 hex chars + NUL */
int  url_decode(const char *in, char *out, size_t outsz);       /* %XX + '+' -> space; returns len or -1 */

/* buffered reader over a socket fd */
typedef struct { int fd; char buf[16384]; size_t start, end; } rio;
void rio_init(rio *r, int fd);
int  rio_read_line(rio *r, char *out, size_t max);              /* strips CRLF; returns len or -1 */
int  rio_read_n(rio *r, char *out, size_t n);                   /* exactly n bytes; 0 ok, -1 err */

/* ---------------- RESP protocol (proto.c) ---------------- */
int  resp_read_command(rio *r, char ***argv_out, int *argc_out); /* 0 ok, -1 eof/err */
void resp_free_argv(char **argv, int argc);
int  resp_dispatch(int argc, char **argv, int fd);             /* 1 continue, 0 close (QUIT) */
void serve_resp(int fd);

/* ---------------- HTTP gateway (http.c) ---------------- */
void serve_http(int fd);

#endif
