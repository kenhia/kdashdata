/**
 * @file kdash_conn.h
 * The connection handle every reader runs on: lazy connect, bounded timeouts,
 * reconnect backoff, and failures swallowed and surfaced as a reachability
 * flag. CD-6 — degrade, don't block — implemented once here instead of three
 * times in three dashboards.
 *
 * The discipline, inherited from kdeskdash/kpidash and unchanged because it is
 * already the right answer for a single-threaded LVGL loop:
 *
 *   - Nothing connects at init. The first read connects; a failed connect arms
 *     a backoff so an unreachable Redis costs one connect timeout per backoff
 *     interval, not one per call.
 *   - Short timeouts (250 ms connect, 50 ms read) — a slow endpoint must not
 *     become a janky panel.
 *   - The endpoint is re-resolved through khlenv on every connect (CD-4), so a
 *     Redis that moved is picked up within one backoff interval.
 *   - Every failure is swallowed. Readers return ABSENT/UNAVAIL; nothing
 *     aborts, longjmps, or logs to stderr on the render path.
 *
 * Consumers are READ-ONLY (CD-5). This handle exposes no write path.
 *
 * Handles are independent: give each endpoint its own, and a stall or backoff
 * on one never reaches another. Single-threaded; no locking.
 */
#ifndef KDASH_CONN_H
#define KDASH_CONN_H

#include <stdbool.h>
#include <stddef.h>

#include "kdash/kdash_endpoint.h"

typedef struct kdash_conn kdash_conn_t;

typedef struct {
    /* khlenv app token for endpoint resolution — the `app` half of
     * KEY.<host>.<app>. NULL or "" means "kdash". */
    const char *app;

    /* Explicit endpoint override. When set, khlenv is never consulted: this is
     * the config-beats-discovery rule CD-4 inherits from kpidash-client, and it
     * is what lets an install pin an endpoint permanently. */
    const char *host;
    int port; /* 0 -> 6379 */

    /* Redis password. NULL reads $REDISCLI_AUTH (CD-2, the contract); an empty
     * string means "no AUTH", which is what a local Redis wants. */
    const char *auth;

    int connect_timeout_ms;  /* 0 -> 250 */
    int read_timeout_ms;     /* 0 -> 50  */
    int reconnect_backoff_s; /* 0 -> 5   */
} kdash_conn_opts_t;

/* Allocate a handle. `opts` may be NULL for all defaults. Never connects.
 * Returns NULL only on allocation failure. */
kdash_conn_t *kdash_conn_new(const kdash_conn_opts_t *opts);

/* Close and free (NULL-safe). */
void kdash_conn_free(kdash_conn_t *c);

/* Ensure a live connection, honouring the backoff and re-resolving the
 * endpoint. True when connected. Readers call this for you; call it directly
 * only to warm the connection at a moment of your choosing. */
bool kdash_conn_ensure(kdash_conn_t *c);

/* Whether the endpoint answered on the most recent attempt. False before the
 * first success and after any connect/read failure — this is the single
 * app-wide "unavailable" signal a dashboard should render, rather than
 * inferring one from an empty result. */
bool kdash_conn_reachable(const kdash_conn_t *c);

/* Endpoint currently in use, as of the last resolve. KDASH_EP_NONE means
 * khlenv holds an explicit null and there is deliberately nothing to connect
 * to — a legitimate configuration, not a fault to retry harder. */
kdash_endpoint_status_t kdash_conn_endpoint(const kdash_conn_t *c, char *host,
                                            size_t hostsz, int *port);

/* Drop the connection; the next read reconnects (after the backoff). */
void kdash_conn_disconnect(kdash_conn_t *c);

#endif /* KDASH_CONN_H */
