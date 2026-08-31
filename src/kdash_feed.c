/**
 * @file kdash_feed.c
 * Typed readers: Redis I/O plus the pure key/payload cores. This unit and
 * kdash_conn.c are the only ones that see hiredis.
 */
#include "kdash/kdash_feed.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kdash_conn_internal.h"

/* Longest payload we will pull for one record. The biggest of the five is a
 * telemetry sample with a disks array; kpidash's are well under 2 KB. */
#define VALUE_MAX 8192

/* ---- shared helpers ------------------------------------------------------ */

/* GET one key. KDASH_OK hands back a borrowed reply the caller must free;
 * every other status has already freed (or never took) one. */
static kdash_status_t get_string(kdash_conn_t *c, const char *key,
                                 redisReply **out) {
    *out = NULL;
    if (!kdash_conn_ensure(c))
        return KDASH_UNAVAIL;

    redisReply *r = redisCommand(c->ctx, "GET %s", key);
    if (!r) {
        /* The context is in error now; ensure() will rebuild it after the
         * backoff. Report unreachable rather than absent — an absent key and a
         * dead socket mean opposite things to a panel. */
        kdash_conn_fail(c);
        return KDASH_UNAVAIL;
    }
    if (r->type != REDIS_REPLY_STRING || r->len == 0) {
        freeReplyObject(r);
        return KDASH_ABSENT; /* missing, expired, or the wrong type */
    }
    *out = r;
    return KDASH_OK;
}

/* Run one bounded SCAN pass, handing every matching key to `visit`. Returns
 * false when the endpoint was unreachable. `visit` returns false to stop the
 * pass early (its output buffer is full). */
typedef bool (*scan_visit_fn)(const char *key, size_t keylen, void *ctx);

static bool scan_keys(kdash_conn_t *c, const char *match, scan_visit_fn visit,
                      void *vctx) {
    if (!kdash_conn_ensure(c))
        return false;

    unsigned long long cursor = 0;
    for (int batch = 0; batch < KDASH_SCAN_BATCHES; batch++) {
        char curbuf[32];
        snprintf(curbuf, sizeof(curbuf), "%llu", cursor);
        redisReply *r = redisCommand(c->ctx, "SCAN %s MATCH %s COUNT %d", curbuf,
                                     match, KDASH_SCAN_COUNT);
        if (!r) {
            kdash_conn_fail(c);
            return false;
        }
        if (r->type != REDIS_REPLY_ARRAY || r->elements != 2 ||
            !r->element[0] || r->element[0]->type != REDIS_REPLY_STRING) {
            freeReplyObject(r);
            return false;
        }

        cursor = strtoull(r->element[0]->str, NULL, 10);
        redisReply *keys = r->element[1];
        bool keep_going = true;
        if (keys && keys->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < keys->elements; i++) {
                redisReply *k = keys->element[i];
                if (!k || k->type != REDIS_REPLY_STRING)
                    continue;
                if (!visit(k->str, (size_t)k->len, vctx)) {
                    keep_going = false;
                    break;
                }
            }
        }
        freeReplyObject(r);

        if (!keep_going || cursor == 0)
            break;
        /* Falling out of the loop on KDASH_SCAN_BATCHES is deliberate: a
         * keyspace big enough to need more passes than that is not one a
         * dashboard should be walking on its render thread. */
    }
    return true;
}

/* ---- kpidash:clients ----------------------------------------------------- */

int kdash_clients(kdash_conn_t *c, char out[][KDASH_TOKEN_MAX], int max,
                  int *skipped) {
    if (skipped)
        *skipped = 0;
    if (!c || !out || max <= 0)
        return 0;
    if (!kdash_conn_ensure(c))
        return -1;

    redisReply *r = redisCommand(c->ctx, "SMEMBERS %s", KDASH_KEY_CLIENTS);
    if (!r) {
        kdash_conn_fail(c);
        return -1;
    }
    if (r->type != REDIS_REPLY_ARRAY) {
        freeReplyObject(r);
        return 0; /* absent set: reachable, nobody publishing */
    }

    int n = 0;
    for (size_t i = 0; i < r->elements && n < max; i++) {
        redisReply *m = r->element[i];
        if (!m || m->type != REDIS_REPLY_STRING) {
            if (skipped)
                (*skipped)++;
            continue;
        }
        /* The set holds bare hostnames, so the token contract is the whole
         * grammar here — and it is the only thing standing between a member
         * somebody typed by hand and a constructed key. */
        if (!kdash_token_ok(m->str, (size_t)m->len) ||
            (size_t)m->len + 1 > KDASH_TOKEN_MAX) {
            if (skipped)
                (*skipped)++;
            continue;
        }
        memcpy(out[n], m->str, (size_t)m->len);
        out[n][m->len] = '\0';
        n++;
    }
    freeReplyObject(r);
    return n;
}

/* ---- per-host client feeds ----------------------------------------------- */

/* Shared body for the three expiring per-host feeds: build the key from a
 * revalidated host, GET it, hand the bytes to the pure parser. */
static kdash_status_t get_client_feed(kdash_conn_t *c, const char *host,
                                      kdash_client_feed_t feed,
                                      bool (*parse)(const char *, size_t, void *),
                                      void *out) {
    if (!c || !host || !out)
        return KDASH_ABSENT;

    char key[KDASH_KEY_MAX];
    if (!kdash_client_key(key, sizeof(key), host, feed))
        return KDASH_ABSENT; /* a host that fails the contract has no key */

    redisReply *r = NULL;
    kdash_status_t st = get_string(c, key, &r);
    if (st != KDASH_OK)
        return st;

    size_t len = (size_t)r->len;
    if (len > VALUE_MAX)
        len = VALUE_MAX;
    bool ok = parse(r->str, len, out);
    freeReplyObject(r);
    return ok ? KDASH_OK : KDASH_ABSENT;
}

static bool parse_health_v(const char *j, size_t n, void *out) {
    return kdash_parse_health(j, n, (kdash_health_t *)out);
}
static bool parse_telemetry_v(const char *j, size_t n, void *out) {
    return kdash_parse_telemetry(j, n, (kdash_telemetry_t *)out);
}
static bool parse_dev_telemetry_v(const char *j, size_t n, void *out) {
    return kdash_parse_dev_telemetry(j, n, (kdash_dev_telemetry_t *)out);
}

kdash_status_t kdash_health(kdash_conn_t *c, const char *host,
                            kdash_health_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    return get_client_feed(c, host, KDASH_CLIENT_HEALTH, parse_health_v, out);
}

kdash_status_t kdash_telemetry(kdash_conn_t *c, const char *host,
                               kdash_telemetry_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    return get_client_feed(c, host, KDASH_CLIENT_TELEMETRY, parse_telemetry_v,
                           out);
}

kdash_status_t kdash_dev_telemetry(kdash_conn_t *c, const char *host,
                                   kdash_dev_telemetry_t *out) {
    if (out)
        memset(out, 0, sizeof(*out));
    return get_client_feed(c, host, KDASH_CLIENT_DEV_TELEMETRY,
                           parse_dev_telemetry_v, out);
}

/* ---- ts-owned families (SCAN + GET) -------------------------------------- */

/* Collected keys for one SCAN pass. The keys are gathered first and read
 * after, because issuing a GET mid-SCAN on the same connection would
 * interleave with the cursor's own replies. */
#define SCAN_KEYS_MAX 256

typedef struct {
    char keys[SCAN_KEYS_MAX][KDASH_KEY_MAX];
    int n;
    int skipped;
    int max; /* stop collecting once the caller's buffer would be full */
} keyset_t;

static bool collect_key(const char *key, size_t keylen, void *vctx) {
    keyset_t *ks = (keyset_t *)vctx;
    if (ks->n >= SCAN_KEYS_MAX || ks->n >= ks->max)
        return false;
    if (keylen + 1 > KDASH_KEY_MAX) {
        ks->skipped++;
        return true;
    }
    memcpy(ks->keys[ks->n], key, keylen);
    ks->keys[ks->n][keylen] = '\0';
    ks->n++;
    return true;
}

int kdash_services(kdash_conn_t *c, kdash_service_t *out, int max,
                   int *skipped) {
    if (skipped)
        *skipped = 0;
    if (!c || !out || max <= 0)
        return 0;

    keyset_t ks = {.max = max};
    if (!scan_keys(c, KDASH_KEY_SERVICES_PFX "*:*", collect_key, &ks))
        return -1;

    int n = 0;
    for (int i = 0; i < ks.n && n < max; i++) {
        kdash_service_t *s = &out[n];
        memset(s, 0, sizeof(*s));
        size_t keylen = strlen(ks.keys[i]);
        /* The choke point: a key that is not exactly 4 segments, or whose
         * name/host fails the token contract, is skipped here and never
         * reaches a GET. */
        if (!kdash_service_key_parse(ks.keys[i], keylen, s->name, sizeof(s->name),
                                     s->host, sizeof(s->host))) {
            ks.skipped++;
            continue;
        }

        redisReply *r = NULL;
        kdash_status_t st = get_string(c, ks.keys[i], &r);
        if (st == KDASH_UNAVAIL)
            return n > 0 ? n : -1;
        if (st != KDASH_OK) {
            ks.skipped++;
            continue; /* raced with an expiry or a delete */
        }
        size_t len = (size_t)r->len;
        if (len > VALUE_MAX)
            len = VALUE_MAX;
        bool ok = kdash_parse_service(r->str, len, s);
        freeReplyObject(r);
        if (ok)
            n++;
        else
            ks.skipped++;
    }

    if (skipped)
        *skipped = ks.skipped;
    return n;
}

int kdash_apttemps(kdash_conn_t *c, kdash_apttemps_t *out, int max,
                   int *skipped) {
    if (skipped)
        *skipped = 0;
    if (!c || !out || max <= 0)
        return 0;

    keyset_t ks = {.max = max};
    if (!scan_keys(c, KDASH_KEY_APTTEMPS_PFX "*", collect_key, &ks))
        return -1;

    int n = 0;
    for (int i = 0; i < ks.n && n < max; i++) {
        kdash_apttemps_t *a = &out[n];
        memset(a, 0, sizeof(*a));
        size_t keylen = strlen(ks.keys[i]);
        if (!kdash_apttemps_key_parse(ks.keys[i], keylen, a->zone,
                                      sizeof(a->zone))) {
            ks.skipped++;
            continue;
        }

        redisReply *r = NULL;
        kdash_status_t st = get_string(c, ks.keys[i], &r);
        if (st == KDASH_UNAVAIL)
            return n > 0 ? n : -1;
        if (st != KDASH_OK) {
            ks.skipped++;
            continue;
        }
        size_t len = (size_t)r->len;
        if (len > VALUE_MAX)
            len = VALUE_MAX;
        bool ok = kdash_parse_apttemps(r->str, len, a);
        freeReplyObject(r);
        if (ok)
            n++;
        else
            ks.skipped++;
    }

    if (skipped)
        *skipped = ks.skipped;
    return n;
}
