/* dict.c - a minimal separate-chaining hash table (string -> string + expiry).
 * This is the data structure a real Redis keeps in memory; here it is plain
 * (no locking, no expiry policy) - store.c layers thread-safety + TTL on top. */
#include <stdlib.h>
#include <string.h>
#include "mr.h"

/* FNV-1a hash */
static size_t hash_key(const char *s) {
    size_t h = 1469598103934665603ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}

dict *dict_new(size_t nbuckets) {
    dict *d = calloc(1, sizeof(dict));
    d->nbuckets = nbuckets;
    d->buckets = calloc(nbuckets, sizeof(dictEntry *));
    d->count = 0;
    return d;
}

dictEntry *dict_find(dict *d, const char *key) {
    size_t i = hash_key(key) % d->nbuckets;
    for (dictEntry *e = d->buckets[i]; e; e = e->next)
        if (strcmp(e->key, key) == 0) return e;
    return NULL;
}

void dict_set(dict *d, const char *key, const char *val, long long expire_at_ms) {
    dictEntry *e = dict_find(d, key);
    if (e) {                                  /* overwrite existing */
        free(e->val);
        e->val = strdup(val);
        e->expire_at_ms = expire_at_ms;
        return;
    }
    size_t i = hash_key(key) % d->nbuckets;
    e = malloc(sizeof(dictEntry));
    e->key = strdup(key);
    e->val = strdup(val);
    e->expire_at_ms = expire_at_ms;
    e->next = d->buckets[i];
    d->buckets[i] = e;
    d->count++;
}

int dict_del(dict *d, const char *key) {
    size_t i = hash_key(key) % d->nbuckets;
    dictEntry **pp = &d->buckets[i];
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            dictEntry *dead = *pp;
            *pp = dead->next;
            free(dead->key); free(dead->val); free(dead);
            d->count--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

void dict_clear(dict *d) {
    for (size_t i = 0; i < d->nbuckets; i++) {
        dictEntry *e = d->buckets[i];
        while (e) { dictEntry *n = e->next; free(e->key); free(e->val); free(e); e = n; }
        d->buckets[i] = NULL;
    }
    d->count = 0;
}
