/* proto.c - RESP protocol: parse one command, dispatch it, write the reply.
 * This is the wire format real Redis clients (redis-cli, Lettuce) speak. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mr.h"

#define MAX_ARGS   1024
#define MAX_BULK   (16 * 1024 * 1024)

/* ---- reply writers ---- */
static void reply_simple(int fd, const char *s) { char b[256]; int n = snprintf(b, sizeof(b), "+%s\r\n", s); write_all(fd, b, n); }
static void reply_error (int fd, const char *s) { char b[256]; int n = snprintf(b, sizeof(b), "-%s\r\n", s); write_all(fd, b, n); }
static void reply_int   (int fd, long long v)   { char b[64];  int n = snprintf(b, sizeof(b), ":%lld\r\n", v); write_all(fd, b, n); }
static void reply_null  (int fd)                { write_all(fd, "$-1\r\n", 5); }
static void reply_bulk  (int fd, const char *s) {
    char hdr[32]; int n = snprintf(hdr, sizeof(hdr), "$%zu\r\n", strlen(s));
    write_all(fd, hdr, n); write_all(fd, s, strlen(s)); write_all(fd, "\r\n", 2);
}

/* ---- read one command into argv ---- */
int resp_read_command(rio *r, char ***argv_out, int *argc_out) {
    char line[1024];
    int n = rio_read_line(r, line, sizeof(line));
    if (n < 0) return -1;
    if (n == 0) { *argv_out = NULL; *argc_out = 0; return 0; }   /* blank line */

    if (line[0] != '*') {                                       /* inline command */
        char **argv = calloc(MAX_ARGS, sizeof(char *));
        int argc = 0;
        char *save, *tok = strtok_r(line, " \t", &save);
        while (tok && argc < MAX_ARGS) { argv[argc++] = strdup(tok); tok = strtok_r(NULL, " \t", &save); }
        *argv_out = argv; *argc_out = argc;
        return 0;
    }

    int count = atoi(line + 1);
    if (count <= 0 || count > MAX_ARGS) return -1;
    char **argv = calloc(count, sizeof(char *));
    for (int i = 0; i < count; i++) {
        if (rio_read_line(r, line, sizeof(line)) < 0 || line[0] != '$') { resp_free_argv(argv, i); return -1; }
        long len = atol(line + 1);
        if (len < 0 || len > MAX_BULK) { resp_free_argv(argv, i); return -1; }
        char *s = malloc((size_t)len + 1);
        if (rio_read_n(r, s, (size_t)len) < 0) { free(s); resp_free_argv(argv, i); return -1; }
        s[len] = 0;
        char crlf[2];
        rio_read_n(r, crlf, 2);                                 /* consume trailing \r\n */
        argv[i] = s;
    }
    *argv_out = argv; *argc_out = count;
    return 0;
}

void resp_free_argv(char **argv, int argc) {
    if (!argv) return;
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv);
}

/* SET key value [NX] [EX s | PX ms] */
static void cmd_set(int argc, char **argv, int fd) {
    if (argc < 3) { reply_error(fd, "ERR wrong number of arguments for 'set'"); return; }
    long long ttl = 0; int nx = 0;
    for (int i = 3; i < argc; i++) {
        if (!strcasecmp(argv[i], "NX")) nx = 1;
        else if (!strcasecmp(argv[i], "EX") && i + 1 < argc) { ttl = atoll(argv[++i]) * 1000; }
        else if (!strcasecmp(argv[i], "PX") && i + 1 < argc) { ttl = atoll(argv[++i]); }
        else if (!strcasecmp(argv[i], "XX")) { /* ignored */ }
        else { reply_error(fd, "ERR syntax error"); return; }
    }
    if (store_set(argv[1], argv[2], ttl, nx)) reply_simple(fd, "OK");
    else reply_null(fd);                                        /* NX failed */
}

/* returns 1 to keep the connection, 0 to close (QUIT) */
int resp_dispatch(int argc, char **argv, int fd) {
    if (argc == 0) return 1;
    const char *c = argv[0];

    if (!strcasecmp(c, "PING")) { if (argc > 1) reply_bulk(fd, argv[1]); else reply_simple(fd, "PONG"); }
    else if (!strcasecmp(c, "ECHO")) { if (argc == 2) reply_bulk(fd, argv[1]); else reply_error(fd, "ERR wrong number of arguments"); }
    else if (!strcasecmp(c, "SET")) cmd_set(argc, argv, fd);
    else if (!strcasecmp(c, "GET")) {
        if (argc != 2) { reply_error(fd, "ERR wrong number of arguments"); return 1; }
        char *v = store_get(argv[1]);
        if (v) { reply_bulk(fd, v); free(v); } else reply_null(fd);
    }
    else if (!strcasecmp(c, "DEL")) {
        long long n = 0; for (int i = 1; i < argc; i++) if (store_del(argv[i])) n++;
        reply_int(fd, n);
    }
    else if (!strcasecmp(c, "EXISTS")) {
        long long n = 0; for (int i = 1; i < argc; i++) if (store_exists(argv[i])) n++;
        reply_int(fd, n);
    }
    else if (!strcasecmp(c, "EXPIRE")) {
        if (argc != 3) { reply_error(fd, "ERR wrong number of arguments"); return 1; }
        reply_int(fd, store_expire(argv[1], atoll(argv[2]) * 1000) ? 1 : 0);
    }
    else if (!strcasecmp(c, "TTL")) {
        if (argc != 2) { reply_error(fd, "ERR wrong number of arguments"); return 1; }
        reply_int(fd, store_ttl(argv[1]));
    }
    else if (!strcasecmp(c, "INCR") || !strcasecmp(c, "DECR") ||
             !strcasecmp(c, "INCRBY") || !strcasecmp(c, "DECRBY")) {
        long long delta, out;
        if (!strcasecmp(c, "INCR")) delta = 1;
        else if (!strcasecmp(c, "DECR")) delta = -1;
        else { if (argc != 3) { reply_error(fd, "ERR wrong number of arguments"); return 1; }
               delta = atoll(argv[2]); if (!strcasecmp(c, "DECRBY")) delta = -delta; }
        const char *key = (!strcasecmp(c, "INCR") || !strcasecmp(c, "DECR")) ? (argc == 2 ? argv[1] : NULL) : argv[1];
        if (!key) { reply_error(fd, "ERR wrong number of arguments"); return 1; }
        if (store_incrby(key, delta, &out)) reply_int(fd, out);
        else reply_error(fd, "ERR value is not an integer or out of range");
    }
    else if (!strcasecmp(c, "KEYS")) {
        size_t n; char **k = store_keys(&n);
        char hdr[32]; int hn = snprintf(hdr, sizeof(hdr), "*%zu\r\n", n); write_all(fd, hdr, hn);
        for (size_t i = 0; i < n; i++) reply_bulk(fd, k[i]);
        store_free_keys(k, n);
    }
    else if (!strcasecmp(c, "DBSIZE")) { size_t n; char **k = store_keys(&n); store_free_keys(k, n); reply_int(fd, (long long)n); }
    else if (!strcasecmp(c, "FLUSHALL") || !strcasecmp(c, "FLUSHDB")) { store_flush(); reply_simple(fd, "OK"); }
    else if (!strcasecmp(c, "INFO")) {
        Stats s = store_stats();
        char b[512]; int n = snprintf(b, sizeof(b),
            "# Server\r\nname:mini-redis-c\r\nversion:1.0.0\r\nuptime_in_seconds:%lld\r\n"
            "# Stats\r\ncommands_processed:%lld\r\nkeyspace_hits:%lld\r\nkeyspace_misses:%lld\r\n"
            "expired_keys:%lld\r\ndb0:keys=%lld\r\n",
            s.uptime_s, s.commands, s.hits, s.misses, s.expired, s.keys);
        reply_bulk(fd, b); (void)n;
    }
    else if (!strcasecmp(c, "COMMAND")) { write_all(fd, "*0\r\n", 4); }
    else if (!strcasecmp(c, "QUIT")) { reply_simple(fd, "OK"); return 0; }
    else reply_error(fd, "ERR unknown command");
    return 1;
}

void serve_resp(int fd) {
    rio r; rio_init(&r, fd);
    for (;;) {
        char **argv; int argc;
        if (resp_read_command(&r, &argv, &argc) < 0) break;
        if (argc > 0) store_count_command();
        int keep = resp_dispatch(argc, argv, fd);
        resp_free_argv(argv, argc);
        if (!keep) break;
    }
}
