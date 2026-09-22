/**
 * @file kdash_feed_internal.h
 * The I/O seam the typed readers run on: SCAN, GET, HGETALL and the free that
 * matches them, behind four function pointers.
 *
 * It exists for exactly one reason. The counted readers publish a contract
 * their own code could not be held to:
 *
 *   -1 means the read did not complete. `out` is zeroed and `*skipped` is 0.
 *
 * Triggering that means losing the endpoint BETWEEN a SCAN and one of the
 * per-key reads that follow it, and there is no way to ask a real Redis for
 * that. Against the live fleet it would mean killing a service three
 * dashboards read; `just check` runs no Redis at all. So from sprint 009 to
 * sprint 016 the rule was enforced by inspection and by a live no-regression
 * run, and the next person to touch those loops had no way to find out they
 * had broken the failure path -- which is the path the whole contract is
 * about (WI 2246).
 *
 * This is a deliberate, named narrowing of CD-10, which keeps the I/O shell
 * out of `just check`. What it buys is that the shell's ONE rule with real
 * consequences is now inside it; what it costs is a test-only indirection
 * here. See docs/architecture.md, CD-10.
 *
 * Three rules keep the cost where it belongs:
 *
 *   - **Nothing public.** These symbols are not in `include/kdash/`, so a
 *     dashboard linking libkdash sees the same API it saw before.
 *   - **The default is the real thing**, installed at load with no
 *     initialisation call, so a consumer that never hears of this file gets
 *     hiredis. `kdash_feed_set_io(NULL)` puts it back.
 *   - **A reply the seam produced is freed by the seam.** That is what lets a
 *     fake own its own allocations instead of depending on hiredis' allocator
 *     matching the test's.
 *
 * Its boundary is worth stating: `kdash_clients()` (SMEMBERS) and
 * `kdash_claude_recent()` (LRANGE) do NOT go through it. They are single round
 * trips, so the mid-list rule is vacuous for them (kdash_feed.h) and there is
 * nothing here they would make testable.
 *
 * Include this only from kdash_feed.c and its tests.
 */
#ifndef KDASH_FEED_INTERNAL_H
#define KDASH_FEED_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

#include "kdash/kdash_feed.h"
#include "kdash_conn_internal.h" /* redisReply, and hiredis in one place */

/* One key from a SCAN pass. Return false to stop the pass early -- the
 * visitor's output buffer is full. */
typedef bool (*kdash_scan_visit_fn)(const char *key, size_t keylen, void *ctx);

typedef struct kdash_feed_io {
    /* One bounded SCAN pass over `match`. False means the endpoint was
     * unreachable, which is what every counted reader turns into -1. */
    bool (*scan_keys)(kdash_conn_t *c, const char *match,
                      kdash_scan_visit_fn visit, void *ctx);

    /* GET one key. KDASH_OK hands back a reply the caller frees with
     * `free_reply`; every other status has already freed (or never took) one. */
    kdash_status_t (*get_string)(kdash_conn_t *c, const char *key,
                                 redisReply **out);

    /* HGETALL one key, same ownership rule. A missing hash is an empty array,
     * i.e. KDASH_ABSENT. */
    kdash_status_t (*get_hash)(kdash_conn_t *c, const char *key,
                               redisReply **out);

    /* Frees what this seam's `get_string`/`get_hash` handed back. NULL-safe,
     * like the `freeReplyObject` it stands in for. */
    void (*free_reply)(void *reply);
} kdash_feed_io_t;

/* The seam currently installed, and the real one. Equal unless a test has
 * swapped it -- which is itself worth asserting, so both are readable. */
const kdash_feed_io_t *kdash_feed_io(void);
const kdash_feed_io_t *kdash_feed_io_real(void);

/* Swap the seam. NULL restores the real implementation. Test-only: nothing in
 * this library calls it, and a caller that does owns the consequences. */
void kdash_feed_set_io(const kdash_feed_io_t *io);

#endif /* KDASH_FEED_INTERNAL_H */
