/* util.c - small shared helpers: time, full-write, random token, URL decode,
 * and a buffered reader (rio) over a socket fd. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include "mr.h"

long long now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; }
        off += (size_t)w;
    }
    return 0;
}

/* 12 random bytes -> 24 hex chars (for lock tokens). */
void gen_token(char out[25]) {
    static const char hx[] = "0123456789abcdef";
    unsigned char b[12];
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) { if (fread(b, 1, 12, f) != 12) { /* fallback below */ } fclose(f); }
    else { for (int i = 0; i < 12; i++) b[i] = (unsigned char)(now_ms() >> i); }
    for (int i = 0; i < 12; i++) { out[i*2] = hx[b[i] >> 4]; out[i*2+1] = hx[b[i] & 15]; }
    out[24] = 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int url_decode(const char *in, char *out, size_t outsz) {
    size_t o = 0;
    for (size_t i = 0; in[i]; i++) {
        if (o + 1 >= outsz) return -1;
        char c = in[i];
        if (c == '+') out[o++] = ' ';
        else if (c == '%' && in[i+1] && in[i+2]) {
            int h = hexval(in[i+1]), l = hexval(in[i+2]);
            if (h < 0 || l < 0) return -1;
            out[o++] = (char)((h << 4) | l);
            i += 2;
        } else out[o++] = c;
    }
    out[o] = 0;
    return (int)o;
}

/* ---------------- buffered reader ---------------- */
void rio_init(rio *r, int fd) { r->fd = fd; r->start = r->end = 0; }

static int rio_fill(rio *r) {
    if (r->start == r->end) { r->start = r->end = 0; }
    if (r->end == sizeof(r->buf)) {
        if (r->start > 0) {
            memmove(r->buf, r->buf + r->start, r->end - r->start);
            r->end -= r->start; r->start = 0;
        } else return -1;                       /* line too long for buffer */
    }
    ssize_t n = read(r->fd, r->buf + r->end, sizeof(r->buf) - r->end);
    if (n <= 0) { if (n < 0 && errno == EINTR) return rio_fill(r); return -1; }
    r->end += (size_t)n;
    return 0;
}

int rio_read_line(rio *r, char *out, size_t max) {
    size_t o = 0;
    for (;;) {
        while (r->start < r->end) {
            char c = r->buf[r->start++];
            if (c == '\n') {
                if (o > 0 && out[o-1] == '\r') o--;
                out[o] = 0;
                return (int)o;
            }
            if (o < max - 1) out[o++] = c;
        }
        if (rio_fill(r) < 0) return -1;
    }
}

int rio_read_n(rio *r, char *out, size_t n) {
    size_t o = 0;
    while (o < n) {
        if (r->start == r->end) { if (rio_fill(r) < 0) return -1; }
        size_t avail = r->end - r->start;
        size_t take = avail < (n - o) ? avail : (n - o);
        memcpy(out + o, r->buf + r->start, take);
        r->start += take; o += take;
    }
    return 0;
}
