/* store.c - thread-safe key/value store with TTL, built on dict.
 * A single global instance guarded by one mutex (thread-per-connection model).
 * Real Redis gets the same safety from a single-threaded event loop instead. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "mr.h"

static dict *D;
static pthread_mutex_t MU = PTHREAD_MUTEX_INITIALIZER;
static long long started_ms;
static unsigned long long g_commands, g_hits, g_misses, g_expired;

static int is_int(const char *s, long long *out) {
    if (!*s) return 0;
    char *end;
    long long v = strtoll(s, &end, 10);
    if (*end) return 0;
    *out = v; return 1;
}

/* caller must hold MU. Returns entry if present & not expired, else NULL (deletes if expired). */
static dictEntry *live(const char *key) {
    dictEntry *e = dict_find(D, key);
    if (!e) return NULL;
    if (e->expire_at_ms != 0 && now_ms() >= e->expire_at_ms) {
        dict_del(D, key);
        g_expired++;
        return NULL;
    }
    return e;
}

void store_init(void) {
    D = dict_new(4096);
    started_ms = now_ms();
}

int store_set(const char *key, const char *val, long long ttl_ms, int nx) {
    pthread_mutex_lock(&MU);
    if (nx && live(key)) { pthread_mutex_unlock(&MU); return 0; }  /* held */
    long long exp = ttl_ms > 0 ? now_ms() + ttl_ms : 0;
    dict_set(D, key, val, exp);
    pthread_mutex_unlock(&MU);
    return 1;
}

char *store_get(const char *key) {
    pthread_mutex_lock(&MU);
    dictEntry *e = live(key);
    char *out = NULL;
    if (e) { out = strdup(e->val); g_hits++; } else { g_misses++; }
    pthread_mutex_unlock(&MU);
    return out;
}

int store_del(const char *key) {
    pthread_mutex_lock(&MU);
    int r = dict_del(D, key);
    pthread_mutex_unlock(&MU);
    return r;
}

int store_del_if_value(const char *key, const char *want) {
    pthread_mutex_lock(&MU);
    dictEntry *e = live(key);
    int r = 0;
    if (e && want && strcmp(e->val, want) == 0) { dict_del(D, key); r = 1; }
    pthread_mutex_unlock(&MU);
    return r;
}

int store_exists(const char *key) {
    pthread_mutex_lock(&MU);
    int r = live(key) != NULL;
    pthread_mutex_unlock(&MU);
    return r;
}

int store_expire(const char *key, long long ttl_ms) {
    pthread_mutex_lock(&MU);
    dictEntry *e = live(key);
    if (e) e->expire_at_ms = ttl_ms > 0 ? now_ms() + ttl_ms : 0;
    int r = e != NULL;
    pthread_mutex_unlock(&MU);
    return r;
}

long long store_ttl(const char *key) {
    pthread_mutex_lock(&MU);
    dictEntry *e = live(key);
    long long r;
    if (!e) r = -2;
    else if (e->expire_at_ms == 0) r = -1;
    else { long long ms = e->expire_at_ms - now_ms(); r = (ms + 999) / 1000; if (r < 0) r = 0; }
    pthread_mutex_unlock(&MU);
    return r;
}

int store_incrby(const char *key, long long delta, long long *out) {
    pthread_mutex_lock(&MU);
    long long cur = 0;
    dictEntry *e = live(key);
    if (e) { if (!is_int(e->val, &cur)) { pthread_mutex_unlock(&MU); return 0; } }
    cur += delta;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", cur);
    long long keep_exp = e ? e->expire_at_ms : 0;
    dict_set(D, key, buf, keep_exp);
    *out = cur;
    pthread_mutex_unlock(&MU);
    return 1;
}

char **store_keys(size_t *n) {
    pthread_mutex_lock(&MU);
    char **arr = malloc(sizeof(char *) * (D->count + 1));
    size_t k = 0;
    long long t = now_ms();
    for (size_t i = 0; i < D->nbuckets; i++)
        for (dictEntry *e = D->buckets[i]; e; e = e->next)
            if (e->expire_at_ms == 0 || t < e->expire_at_ms)
                arr[k++] = strdup(e->key);
    *n = k;
    pthread_mutex_unlock(&MU);
    return arr;
}

void store_free_keys(char **keys, size_t n) {
    for (size_t i = 0; i < n; i++) free(keys[i]);
    free(keys);
}

void store_flush(void) {
    pthread_mutex_lock(&MU);
    dict_clear(D);
    pthread_mutex_unlock(&MU);
}

void store_count_command(void) {
    pthread_mutex_lock(&MU);
    g_commands++;
    pthread_mutex_unlock(&MU);
}

Stats store_stats(void) {
    pthread_mutex_lock(&MU);
    Stats s;
    long long t = now_ms();
    long long live_keys = 0;
    for (size_t i = 0; i < D->nbuckets; i++)
        for (dictEntry *e = D->buckets[i]; e; e = e->next)
            if (e->expire_at_ms == 0 || t < e->expire_at_ms) live_keys++;
    s.uptime_s = (t - started_ms) / 1000;
    s.keys = live_keys;
    s.commands = (long long)g_commands;
    s.hits = (long long)g_hits;
    s.misses = (long long)g_misses;
    s.expired = (long long)g_expired;
    pthread_mutex_unlock(&MU);
    return s;
}

void store_sweep(void) {
    pthread_mutex_lock(&MU);
    long long t = now_ms();
    for (size_t i = 0; i < D->nbuckets; i++) {
        dictEntry **pp = &D->buckets[i];
        while (*pp) {
            if ((*pp)->expire_at_ms != 0 && t >= (*pp)->expire_at_ms) {
                dictEntry *dead = *pp; *pp = dead->next;
                free(dead->key); free(dead->val); free(dead);
                D->count--; g_expired++;
            } else pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&MU);
}
