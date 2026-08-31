/**
 * @file kdash_conn_internal.h
 * Private layout of the connection handle — the ONE place that includes
 * hiredis. Public consumers include kdash_conn.h, where kdash_conn_t is
 * opaque, so a dashboard linking this library needs no hiredis headers of its
 * own and gains no compile-time coupling to it.
 *
 * Include this only from the units that own the connection (kdash_conn.c,
 * kdash_feed.c).
 */
#ifndef KDASH_CONN_INTERNAL_H
#define KDASH_CONN_INTERNAL_H

#include <time.h>

#include <hiredis/hiredis.h>

#include "kdash/kdash_conn.h"
#include "kdash/kdash_endpoint.h"

#define KDASH_APP_MAX  64
#define KDASH_AUTH_MAX 256

struct kdash_conn {
    redisContext *ctx;

    char app[KDASH_APP_MAX];
    char auth[KDASH_AUTH_MAX];

    /* Explicit override, "" when discovery is in charge. */
    char cfg_host[KDASH_ENDPOINT_MAX];
    int cfg_port;

    /* Endpoint of the last resolve, and how that resolve went. */
    char host[KDASH_ENDPOINT_MAX];
    int port;
    kdash_endpoint_status_t ep_status;

    int connect_timeout_ms;
    int read_timeout_ms;
    int reconnect_backoff_s;

    time_t next_attempt;
    bool reachable;
};

/* Mark the endpoint as having failed this attempt and arm the backoff. */
void kdash_conn_fail(kdash_conn_t *c);

#endif /* KDASH_CONN_INTERNAL_H */
