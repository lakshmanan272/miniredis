/* main.c - wiring: a listener for RESP (:6379) and one for HTTP ($PORT),
 * thread-per-connection, plus a background TTL sweeper.
 *   RESP/TCP  -> real Redis clients (redis-cli, Lettuce)
 *   HTTP      -> the gateway a free-tier cloud app uses (BookMyEvent). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "mr.h"

typedef struct { int fd; int is_http; } conn_t;
typedef struct { int lfd; int is_http; } acceptor_t;

static int make_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((unsigned short)port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
    if (listen(fd, 128) < 0) { close(fd); return -1; }
    return fd;
}

static void *handler(void *arg) {
    conn_t *c = arg;
    if (c->is_http) serve_http(c->fd);
    else            serve_resp(c->fd);
    close(c->fd);
    free(c);
    return NULL;
}

static void *acceptor(void *arg) {
    acceptor_t *ac = arg;
    for (;;) {
        int cfd = accept(ac->lfd, NULL, NULL);
        if (cfd < 0) { if (errno == EINTR) continue; break; }
        conn_t *c = malloc(sizeof(conn_t));
        c->fd = cfd; c->is_http = ac->is_http;
        pthread_t t;
        if (pthread_create(&t, NULL, handler, c) != 0) { close(cfd); free(c); continue; }
        pthread_detach(t);
    }
    return NULL;
}

static void *sweeper(void *arg) {
    (void)arg;
    for (;;) { sleep(1); store_sweep(); }
    return NULL;
}

static int env_port(const char *name, int def) {
    const char *v = getenv(name);
    return (v && *v) ? atoi(v) : def;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);                 /* don't die when a client disconnects mid-write */
    setvbuf(stdout, NULL, _IONBF, 0);

    store_init();

    int http_port = env_port("PORT", 8080);   /* Render injects PORT for the web service */
    int resp_port = env_port("RESP_PORT", 6379);

    pthread_t sw;
    pthread_create(&sw, NULL, sweeper, NULL);
    pthread_detach(sw);

    int rfd = make_listener(resp_port);
    if (rfd >= 0) {
        printf("RESP (Redis protocol) listening on :%d\n", resp_port);
        static acceptor_t ra; ra.lfd = rfd; ra.is_http = 0;
        pthread_t t; pthread_create(&t, NULL, acceptor, &ra); pthread_detach(t);
    } else {
        printf("RESP listener on :%d not started (HTTP gateway still runs)\n", resp_port);
    }

    int hfd = make_listener(http_port);
    if (hfd < 0) { fprintf(stderr, "FATAL: cannot bind HTTP port %d\n", http_port); return 1; }
    printf("HTTP gateway + dashboard on :%d\n", http_port);

    static acceptor_t ha; ha.lfd = hfd; ha.is_http = 1;
    acceptor(&ha);                            /* run the HTTP acceptor on the main thread (blocks) */
    return 0;
}
