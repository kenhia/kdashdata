/**
 * @file kdash_conn.c
 * Connection handle: lazy connect, bounded timeouts, reconnect backoff,
 * khlenv resolution on every connect. Synchronous and single-threaded, driven
 * from the caller's poll loop.
 */
#include "kdash_conn_internal.h"

#include <stdlib.h>
#include <string.h>

#define DEFAULT_CONNECT_TIMEOUT_MS 250
#define DEFAULT_READ_TIMEOUT_MS    50
#define DEFAULT_BACKOFF_S          5

static void copy_bounded(char *dst, size_t dstsz, const char *src) {
    dst[0] = '\0';
    if (!src)
        return;
    size_t n = strlen(src);
    if (n + 1 > dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

kdash_conn_t *kdash_conn_new(const kdash_conn_opts_t *opts) {
    kdash_conn_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    const char *app = opts ? opts->app : NULL;
    copy_bounded(c->app, sizeof(c->app), (app && app[0]) ? app : "kdash");

    /* NULL auth means "take the contract's environment variable" (CD-2); an
     * empty string means the caller deliberately wants no AUTH. */
    const char *auth = opts ? opts->auth : NULL;
    if (!auth)
        auth = getenv(KDASH_AUTH_ENV);
    copy_bounded(c->auth, sizeof(c->auth), auth);

    /* NULL stem means the central Redis — what every kpidash reader resolves,
     * and what this handle did before the field existed. */
    const kdash_stem_t *stem = (opts && opts->stem) ? opts->stem : &KDASH_STEM_CENTRAL;
    copy_bounded(c->stem_key, sizeof(c->stem_key), stem->key);
    copy_bounded(c->stem_legacy, sizeof(c->stem_legacy), stem->legacy);
    copy_bounded(c->stem_fallback, sizeof(c->stem_fallback), stem->fallback);
    if (!c->stem_key[0]) {
        /* A stem with no name resolves nothing; refusing here beats sending
         * khlenv an empty key on every reconnect for the life of the handle. */
        free(c);
        return NULL;
    }

    copy_bounded(c->cfg_host, sizeof(c->cfg_host), opts ? opts->host : NULL);
    c->cfg_port = (opts && opts->port > 0) ? opts->port : KDASH_REDIS_PORT_DEFAULT;

    c->connect_timeout_ms = (opts && opts->connect_timeout_ms > 0)
                                ? opts->connect_timeout_ms
                                : DEFAULT_CONNECT_TIMEOUT_MS;
    c->read_timeout_ms = (opts && opts->read_timeout_ms > 0)
                             ? opts->read_timeout_ms
                             : DEFAULT_READ_TIMEOUT_MS;
    c->reconnect_backoff_s = (opts && opts->reconnect_backoff_s > 0)
                                 ? opts->reconnect_backoff_s
                                 : DEFAULT_BACKOFF_S;

    c->ep_status = KDASH_EP_OK;
    c->port = c->cfg_port;
    return c;
}

void kdash_conn_free(kdash_conn_t *c) {
    if (!c)
        return;
    if (c->ctx)
        redisFree(c->ctx);
    free(c);
}

void kdash_conn_disconnect(kdash_conn_t *c) {
    if (!c || !c->ctx)
        return;
    redisFree(c->ctx);
    c->ctx = NULL;
    c->reachable = false;
}

bool kdash_conn_reachable(const kdash_conn_t *c) {
    return c && c->reachable;
}

kdash_endpoint_status_t kdash_conn_endpoint(const kdash_conn_t *c, char *host,
                                            size_t hostsz, int *port) {
    if (!c)
        return KDASH_EP_INVALID;
    if (host && hostsz > 0)
        copy_bounded(host, hostsz, c->host);
    if (port)
        *port = c->port;
    return c->ep_status;
}

void kdash_conn_fail(kdash_conn_t *c) {
    if (!c)
        return;
    if (c->ctx) {
        redisFree(c->ctx);
        c->ctx = NULL;
    }
    c->reachable = false;
    c->next_attempt = time(NULL) + c->reconnect_backoff_s;
}

/* Resolve the endpoint for this attempt. An explicit host on the handle wins
 * outright and skips khlenv entirely; otherwise this is CD-4's resolve, run
 * fresh on every connect so a moved Redis propagates on its own. */
static bool resolve_endpoint(kdash_conn_t *c) {
    if (c->cfg_host[0]) {
        copy_bounded(c->host, sizeof(c->host), c->cfg_host);
        c->port = c->cfg_port;
        c->ep_status = KDASH_EP_OK;
        return true;
    }
    /* Rebuilt per resolve from the handle's own copies, so the walk sees this
     * family's stem and never the central one by accident. */
    const kdash_stem_t stem = {
        .key = c->stem_key,
        .legacy = c->stem_legacy[0] ? c->stem_legacy : NULL,
        .fallback = c->stem_fallback[0] ? c->stem_fallback : NULL,
    };
    c->ep_status =
        kdash_resolve_endpoint(&stem, c->app, c->host, sizeof(c->host), &c->port);
    return c->ep_status == KDASH_EP_OK;
}

bool kdash_conn_ensure(kdash_conn_t *c) {
    if (!c)
        return false;
    if (c->ctx && c->ctx->err == 0)
        return true;

    time_t now = time(NULL);
    if (c->ctx) {
        /* The context errored on a command (a read timeout against a
         * reachable-but-slow endpoint, say). Reconnecting immediately would
         * thrash connect+timeout every tick, so arm the same backoff a failed
         * connect gets before dropping it. */
        redisFree(c->ctx);
        c->ctx = NULL;
        c->reachable = false;
        c->next_attempt = now + c->reconnect_backoff_s;
    }
    if (now < c->next_attempt)
        return false;

    if (!resolve_endpoint(c)) {
        /* Includes KDASH_EP_NONE (a deliberate "no endpoint") and
         * KDASH_EP_UNRESOLVED (nothing anywhere, and this stem would rather
         * resolve nothing than guess). Back off like any other failure so
         * either costs one resolve per interval rather than one per read. */
        c->reachable = false;
        c->next_attempt = now + c->reconnect_backoff_s;
        return false;
    }

    struct timeval tv = {c->connect_timeout_ms / 1000,
                         (c->connect_timeout_ms % 1000) * 1000};
    redisContext *ctx = redisConnectWithTimeout(c->host, c->port, tv);
    if (!ctx || ctx->err) {
        if (ctx)
            redisFree(ctx);
        c->reachable = false;
        c->next_attempt = now + c->reconnect_backoff_s;
        return false;
    }

    struct timeval rtv = {c->read_timeout_ms / 1000,
                          (c->read_timeout_ms % 1000) * 1000};
    if (redisSetTimeout(ctx, rtv) != REDIS_OK) {
        redisFree(ctx);
        c->reachable = false;
        c->next_attempt = now + c->reconnect_backoff_s;
        return false;
    }

    if (c->auth[0]) {
        redisReply *r = redisCommand(ctx, "AUTH %s", c->auth);
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r)
            freeReplyObject(r);
        if (!ok) {
            /* A wrong password is not transient, but it is also not a reason to
             * crash a dashboard: back off and keep rendering "unavailable". */
            redisFree(ctx);
            c->reachable = false;
            c->next_attempt = now + c->reconnect_backoff_s;
            return false;
        }
    }

    /* One PING before declaring the connection usable. Without it a server that
     * accepts the socket but refuses commands — no password supplied to an
     * AUTH-required Redis being the case that actually happens — looks
     * connected, and every read comes back as if the key were simply absent.
     * "Every host is offline" is a far worse answer for a panel than
     * "unavailable", and this is the one round-trip that tells them apart. It
     * costs one exchange per connect, i.e. one per backoff interval at worst. */
    {
        redisReply *r = redisCommand(ctx, "PING");
        bool ok = r && r->type != REDIS_REPLY_ERROR;
        if (r)
            freeReplyObject(r);
        if (!ok) {
            redisFree(ctx);
            c->reachable = false;
            c->next_attempt = now + c->reconnect_backoff_s;
            return false;
        }
    }

    c->ctx = ctx;
    c->reachable = true;
    return true;
}
