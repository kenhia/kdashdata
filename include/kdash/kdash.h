/**
 * @file kdash.h
 * kdashdata's shared C consumer library — the umbrella header.
 *
 * One include for a dashboard that just wants the feeds:
 *
 *   #include <kdash/kdash.h>
 *
 *   kdash_conn_t *c = kdash_conn_new(&(kdash_conn_opts_t){.app = "kstudiodash"});
 *   char hosts[16][KDASH_TOKEN_MAX];
 *   int n = kdash_clients(c, hosts, 16, NULL);
 *   for (int i = 0; i < n; i++) {
 *       kdash_telemetry_t t;
 *       if (kdash_telemetry(c, hosts[i], &t) == KDASH_OK)
 *           render(&t);           // fresh by construction: the key has a TTL
 *   }
 *
 * What this library is: the data model and freshness rules of the feeds in
 * contracts/registry.md, plus a Redis client that degrades instead of
 * blocking. What it is deliberately NOT: any rendering at all. It has no
 * knowledge of LVGL, and nothing here draws, lays out, or owns a screen.
 *
 * Layering, which is also the test story — the first three headers are pure
 * (no Redis, no sockets, no clock) and carry the unit tests; the last two are
 * the thin I/O shell over them:
 *
 *   kdash_keys.h       key grammar and the token choke point      pure
 *   kdash_freshness.h  both freshness models + the CD-6 ladder    pure
 *   kdash_payload.h    the five schema'd payloads                 pure
 *   kdash_endpoint.h   khlenv protocol; CD-4 discovery            pure + I/O
 *   kdash_conn.h       connection, backoff, reachability          I/O
 *   kdash_feed.h       the typed readers                          I/O
 */
#ifndef KDASH_H
#define KDASH_H

#include "kdash/kdash_conn.h"
#include "kdash/kdash_endpoint.h"
#include "kdash/kdash_feed.h"
#include "kdash/kdash_freshness.h"
#include "kdash/kdash_keys.h"
#include "kdash/kdash_payload.h"

#endif /* KDASH_H */
