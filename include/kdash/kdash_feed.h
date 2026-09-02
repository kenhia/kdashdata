/**
 * @file kdash_feed.h
 * Typed readers for the schema'd feeds — the five kpidash ones and the three
 * claude ones. A consumer using these needs no knowledge of key grammar, SCAN,
 * HGETALL, hiredis reply types, or the difference between the two freshness
 * models — which is the whole point of the library existing once instead of
 * three times.
 *
 * Every reader:
 *   - connects lazily through the handle and honours its backoff (CD-6);
 *   - validates each discovered key at the choke point and SKIPS what fails,
 *     counting it rather than crashing or guessing (rules.md);
 *   - parses each payload against its schema, rejecting a record whole when a
 *     required field is missing or malformed, and counting that too;
 *   - is read-only (CD-5).
 *
 * Freshness is deliberately NOT applied here. The expiring feeds answer it by
 * key absence (KDASH_ABSENT), and the ts-owned feeds hand back `ts` so the
 * caller applies its own window with kdash_freshness.h — that is what
 * "reader-owned staleness" means, and a library that pre-filtered stale
 * records would take the decision away from the panel that has to render them.
 */
#ifndef KDASH_FEED_H
#define KDASH_FEED_H

#include <stdbool.h>

#include "kdash/kdash_conn.h"
#include "kdash/kdash_keys.h"
#include "kdash/kdash_payload.h"

typedef enum {
    KDASH_OK = 0,   /* key present and parsed into *out                     */
    KDASH_ABSENT,   /* key missing, expired, empty or malformed (out zeroed) */
    KDASH_UNAVAIL,  /* endpoint unreachable this attempt (out zeroed)        */
} kdash_status_t;

/* Hard caps on one read, so a runaway keyspace cannot stall a render loop.
 * A pass that hits the batch cap returns what it found; it does not error. */
#define KDASH_SCAN_COUNT   100
#define KDASH_SCAN_BATCHES 64

/* Enumerate the hosts publishing client feeds: SMEMBERS kpidash:clients, each
 * member validated against the token contract. Returns the number written, or
 * -1 when the endpoint was unreachable — 0 means "reachable, nobody
 * publishing", which is a different thing a panel must be able to say.
 * `skipped` (optional) counts members that failed the contract. */
int kdash_clients(kdash_conn_t *c, char out[][KDASH_TOKEN_MAX], int max,
                  int *skipped);

/* The three per-host client feeds. `host` is revalidated before a key is
 * built, so a value round-tripped through config or Redis cannot construct a
 * key. Absence is meaningful for all three: these are the expiring feeds, and
 * KDASH_ABSENT means the publisher is offline (its TTL lapsed). */
kdash_status_t kdash_health(kdash_conn_t *c, const char *host,
                            kdash_health_t *out);
kdash_status_t kdash_telemetry(kdash_conn_t *c, const char *host,
                               kdash_telemetry_t *out);
kdash_status_t kdash_dev_telemetry(kdash_conn_t *c, const char *host,
                                   kdash_dev_telemetry_t *out);

/* SCAN kpidash:services:*:* and read every conforming card. Keys that are not
 * exactly 4 segments are ignored (the additive-evolution rule); the `_` host
 * sentinel arrives as an empty `host`. Returns the count written, or -1 when
 * unreachable. `skipped` counts keys rejected by the grammar plus payloads
 * rejected by their schema.
 *
 * Staleness is the caller's: compare each `ts` against
 * KDASH_SERVICES_WINDOW_S. */
int kdash_services(kdash_conn_t *c, kdash_service_t *out, int max, int *skipped);

/* SCAN kpidash:apttemps:* — same contract, KDASH_APTTEMPS_WINDOW_S window. */
int kdash_apttemps(kdash_conn_t *c, kdash_apttemps_t *out, int max,
                   int *skipped);

/* ---- the claude family --------------------------------------------------
 *
 * These live at their OWN endpoint (CD-7), so they need a handle opened on
 * `&KDASH_STEM_CLAUDE` — not the one the kpidash readers run on, even though
 * both stems answer the same host:port today:
 *
 *   kdash_conn_t *cc = kdash_conn_new(&(kdash_conn_opts_t){
 *       .app = "kstudiodash", .stem = &KDASH_STEM_CLAUDE});
 *
 * Handles are independent by design (kdash_conn.h), so a claude endpoint that
 * is down never costs the kpidash panel a thing.
 *
 * All three are ts-owned: nothing is filtered by age here. Derive display
 * state with kdash_claude_sessions_refresh() and judge the limits gauges with
 * kdash_claude_limits_stale() — both in kdash_payload.h, both pure.
 */

/* SCAN claude:session:*, then HGETALL each key that survives the grammar.
 * Returns the count written, or -1 when the endpoint was unreachable; 0 means
 * "reachable, nobody has Claude open", which is a different thing to say.
 * `skipped` counts keys the grammar rejected plus hashes their schema did —
 * a session whose hash carries no `status` is the common one, and it is a
 * legitimate transient rather than a fault (see kdash_parse_claude_session).
 *
 * `disp` is left zeroed; call kdash_claude_sessions_refresh() to derive it and
 * order the rows. */
int kdash_claude_sessions(kdash_conn_t *c, kdash_claude_session_t *out, int max,
                          int *skipped);

/* HGETALL claude:limits — one shared key, no TTL. KDASH_ABSENT covers "nobody
 * has published usage" and "the hash is there but not schema-valid" alike. */
kdash_status_t kdash_claude_limits(kdash_conn_t *c, kdash_claude_limits_t *out);

/* LRANGE claude:recent 0 max-1 — newest first, as the writer's LPUSH leaves
 * it. Returns the count written, or -1 when unreachable; `skipped` counts
 * elements their schema rejected. The writer owns the cap (rules.md): this
 * reader never trims the list. */
int kdash_claude_recent(kdash_conn_t *c, kdash_claude_recent_t *out, int max,
                        int *skipped);

#endif /* KDASH_FEED_H */
