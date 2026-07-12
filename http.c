/* http.c - a minimal HTTP/1.1 gateway so a cloud app (BookMyEvent on Render,
 * where raw Redis TCP is not exposed) can use Mini-Redis over HTTPS.
 * Routes: GET / (dashboard), /healthz, /stats, POST /api/lock/{acquire,release}, /api/cmd. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "mr.h"
#include "dashboard.h"

/* ---- response helper ---- */
static void send_resp(int fd, const char *status, const char *ctype, const char *body) {
    char hdr[512];
    int n = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        status, ctype, strlen(body));
    write_all(fd, hdr, n);
    write_all(fd, body, strlen(body));
}
static void send_json(int fd, const char *status, const char *body) { send_resp(fd, status, "application/json", body); }

/* JSON-escape src into dst */
static void json_escape(const char *src, char *dst, size_t dstsz) {
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 2 < dstsz; i++) {
        unsigned char c = src[i];
        if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = c; }
        else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (c < 0x20) { continue; }
        else dst[o++] = c;
    }
    dst[o] = 0;
}

/* ?key=value&... : find key, url-decode its value into out */
static int query_get(const char *query, const char *key, char *out, size_t outsz) {
    if (!query) return 0;
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *amp = strchr(v, '&');
            char raw[2048];
            size_t vlen = amp ? (size_t)(amp - v) : strlen(v);
            if (vlen >= sizeof(raw)) vlen = sizeof(raw) - 1;
            memcpy(raw, v, vlen); raw[vlen] = 0;
            return url_decode(raw, out, outsz) >= 0;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

/* parse {"args":["a","b",...]} -> argv (very small, tolerant). Returns argc. */
static int parse_args(const char *body, char **argv, int maxargs) {
    const char *p = strstr(body, "\"args\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    int argc = 0;
    while (*p && *p != ']' && argc < maxargs) {
        while (*p == ' ' || *p == ',' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (*p != '"') break;
        p++;
        char tmp[1024]; size_t o = 0;
        while (*p && *p != '"' && o + 1 < sizeof(tmp)) {
            if (*p == '\\' && p[1]) { p++; tmp[o++] = *p; }
            else tmp[o++] = *p;
            p++;
        }
        tmp[o] = 0;
        if (*p == '"') p++;
        argv[argc++] = strdup(tmp);
    }
    return argc;
}
static void free_args(char **argv, int argc) { for (int i = 0; i < argc; i++) free(argv[i]); }

/* small command subset for the dashboard console -> JSON result */
static void http_exec(const char *body, int fd) {
    char *argv[64]; int argc = parse_args(body, argv, 64);
    if (argc == 0) { send_json(fd, "400 Bad Request", "{\"error\":\"send {\\\"args\\\":[...]}\"}"); return; }
    char out[2048]; const char *cmd = argv[0];
    if (!strcasecmp(cmd, "PING")) snprintf(out, sizeof(out), "{\"result\":\"PONG\"}");
    else if (!strcasecmp(cmd, "SET") && argc >= 3) {
        long long ttl = 0; int nx = 0;
        for (int i = 3; i < argc; i++) {
            if (!strcasecmp(argv[i], "NX")) nx = 1;
            else if (!strcasecmp(argv[i], "EX") && i + 1 < argc) ttl = atoll(argv[++i]) * 1000;
            else if (!strcasecmp(argv[i], "PX") && i + 1 < argc) ttl = atoll(argv[++i]);
        }
        int ok = store_set(argv[1], argv[2], ttl, nx);
        snprintf(out, sizeof(out), ok ? "{\"result\":\"OK\"}" : "{\"result\":null}");
    }
    else if (!strcasecmp(cmd, "GET") && argc == 2) {
        char *v = store_get(argv[1]);
        if (v) { char e[1024]; json_escape(v, e, sizeof(e)); snprintf(out, sizeof(out), "{\"result\":\"%s\"}", e); free(v); }
        else snprintf(out, sizeof(out), "{\"result\":null}");
    }
    else if (!strcasecmp(cmd, "DEL") && argc == 2) snprintf(out, sizeof(out), "{\"result\":%s}", store_del(argv[1]) ? "true" : "false");
    else if (!strcasecmp(cmd, "TTL") && argc == 2) snprintf(out, sizeof(out), "{\"result\":%lld}", store_ttl(argv[1]));
    else if (!strcasecmp(cmd, "INCR") && argc == 2) { long long o; store_incrby(argv[1], 1, &o); snprintf(out, sizeof(out), "{\"result\":%lld}", o); }
    else if (!strcasecmp(cmd, "KEYS")) {
        size_t n; char **k = store_keys(&n);
        size_t o = snprintf(out, sizeof(out), "{\"result\":[");
        for (size_t i = 0; i < n && o < sizeof(out) - 64; i++) {
            char e[256]; json_escape(k[i], e, sizeof(e));
            o += snprintf(out + o, sizeof(out) - o, "%s\"%s\"", i ? "," : "", e);
        }
        snprintf(out + o, sizeof(out) - o, "]}");
        store_free_keys(k, n);
    }
    else snprintf(out, sizeof(out), "{\"error\":\"unsupported over HTTP\"}");
    free_args(argv, argc);
    send_json(fd, "200 OK", out);
}

void serve_http(int fd) {
    rio r; rio_init(&r, fd);
    char line[4096];
    if (rio_read_line(&r, line, sizeof(line)) < 0) return;

    char method[16] = {0}, target[2048] = {0};
    sscanf(line, "%15s %2047s", method, target);

    /* split path?query */
    char *query = strchr(target, '?');
    if (query) { *query = 0; query++; }
    char *path = target;

    /* drain headers, note Content-Length */
    long content_length = 0;
    for (;;) {
        int n = rio_read_line(&r, line, sizeof(line));
        if (n <= 0) break;                                  /* blank line or error ends headers */
        if (!strncasecmp(line, "Content-Length:", 15)) content_length = atol(line + 15);
    }
    char body[16384]; body[0] = 0;
    if (content_length > 0) {
        if (content_length > (long)sizeof(body) - 1) content_length = sizeof(body) - 1;
        if (rio_read_n(&r, body, (size_t)content_length) < 0) return;
        body[content_length] = 0;
    }

    store_count_command();

    if (!strcmp(method, "GET") && !strcmp(path, "/")) { send_resp(fd, "200 OK", "text/html; charset=utf-8", DASHBOARD_HTML); return; }
    if (!strcmp(path, "/healthz")) { send_json(fd, "200 OK", "{\"status\":\"UP\"}"); return; }
    if (!strcmp(path, "/stats")) {
        Stats s = store_stats();
        char b[512];
        snprintf(b, sizeof(b),
            "{\"uptime_seconds\":%lld,\"keys\":%lld,\"commands_processed\":%lld,"
            "\"keyspace_hits\":%lld,\"keyspace_misses\":%lld,\"expired_keys\":%lld}",
            s.uptime_s, s.keys, s.commands, s.hits, s.misses, s.expired);
        send_json(fd, "200 OK", b);
        return;
    }
    if (!strcmp(path, "/api/lock/acquire")) {
        if (strcmp(method, "POST")) { send_json(fd, "405 Method Not Allowed", "{\"error\":\"use POST\"}"); return; }
        char key[2048];
        if (!query_get(query, "key", key, sizeof(key)) || !key[0]) { send_json(fd, "400 Bad Request", "{\"error\":\"key required\"}"); return; }
        char ttlbuf[32]; long long ttl = 10000;
        if (query_get(query, "ttl", ttlbuf, sizeof(ttlbuf))) { long long t = atoll(ttlbuf); if (t > 0) ttl = t; }
        char token[25]; gen_token(token);
        int ok = store_set(key, token, ttl, 1 /*nx*/);
        char b[128];
        if (ok) snprintf(b, sizeof(b), "{\"acquired\":true,\"token\":\"%s\"}", token);
        else    snprintf(b, sizeof(b), "{\"acquired\":false}");
        send_json(fd, "200 OK", b);
        return;
    }
    if (!strcmp(path, "/api/lock/release")) {
        if (strcmp(method, "POST")) { send_json(fd, "405 Method Not Allowed", "{\"error\":\"use POST\"}"); return; }
        char key[2048] = {0}, token[2048] = {0};
        query_get(query, "key", key, sizeof(key));
        query_get(query, "token", token, sizeof(token));
        int r2 = store_del_if_value(key, token);
        char b[64]; snprintf(b, sizeof(b), "{\"released\":%s}", r2 ? "true" : "false");
        send_json(fd, "200 OK", b);
        return;
    }
    if (!strcmp(path, "/api/cmd")) {
        if (strcmp(method, "POST")) { send_json(fd, "405 Method Not Allowed", "{\"error\":\"use POST\"}"); return; }
        http_exec(body, fd);
        return;
    }
    send_json(fd, "404 Not Found", "{\"error\":\"not found\"}");
}
