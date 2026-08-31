/**
 * @file kdash_keys.h
 * Pure key grammar for the feeds kdashdata governs. No Redis, no sockets,
 * no allocation — host-testable.
 *
 * This header is the **choke point** required by contracts/rules.md: every
 * key a reader discovers is parsed here before its segments reach a GET, a
 * constructed key, or a UI label. A key that fails the grammar is skipped
 * entirely — never truncated-and-used, never guessed at, never fatal.
 *
 * Families (contracts/registry.md):
 *   kpidash:clients                             SET of host tokens
 *   kpidash:client:<host>:health                3 fixed + 1 host segment
 *   kpidash:client:<host>:telemetry
 *   kpidash:client:<host>:dev_telemetry
 *   kpidash:services:<name>:<host>              exactly 4 segments, `_` = none
 *   kpidash:apttemps:<zone>                     exactly 3 segments
 */
#ifndef KDASH_KEYS_H
#define KDASH_KEYS_H

#include <stdbool.h>
#include <stddef.h>

/* Host/session token contract (rules.md): [A-Za-z0-9._-], 1..63 chars.
 * The buffer size is 63 + NUL. */
#define KDASH_TOKEN_MAX 64

/* Service name and apartment zone slug both ride the same token charset; they
 * are key segments, so they get the same buffer and the same validation. */
#define KDASH_NAME_MAX 64
#define KDASH_ZONE_MAX 64

#define KDASH_KEY_CLIENTS      "kpidash:clients"
#define KDASH_KEY_CLIENT_PFX   "kpidash:client:"
#define KDASH_KEY_SERVICES_PFX "kpidash:services:"
#define KDASH_KEY_APTTEMPS_PFX "kpidash:apttemps:"

/* The `<host>` sentinel meaning "this service has no host" (registry.md). */
#define KDASH_HOST_SENTINEL "_"

/* Longest key any of these families can produce, plus NUL. The client family
 * is the widest: prefix + 63-char host + ":dev_telemetry". */
#define KDASH_KEY_MAX 128

/* Which per-host client feed a key names. */
typedef enum {
    KDASH_CLIENT_HEALTH = 0,
    KDASH_CLIENT_TELEMETRY,
    KDASH_CLIENT_DEV_TELEMETRY,
} kdash_client_feed_t;

/* True when `tok` (length `len`, not necessarily NUL-terminated) satisfies the
 * token contract: non-empty, <= 63 chars, charset [A-Za-z0-9._-] only. */
bool kdash_token_ok(const char *tok, size_t len);

/* The `:`-suffix a client feed uses, e.g. ":dev_telemetry". Never NULL. */
const char *kdash_client_feed_suffix(kdash_client_feed_t feed);

/* Build `kpidash:client:<host>:<feed>` into `out`. Revalidates `host` against
 * the token contract first — a caller may be passing back a value it read from
 * Redis, so construction is a choke point too. False (and `out` untouched) on a
 * bad token or a buffer too small. */
bool kdash_client_key(char *out, size_t outsz, const char *host,
                      kdash_client_feed_t feed);

/* Extract `<host>` from `kpidash:client:<host>:<feed>` of length `keylen`.
 * Anchored exactly at both ends; the host must pass the token contract.
 * True and `host` filled on success; false with `host` untouched otherwise. */
bool kdash_client_key_parse(const char *key, size_t keylen,
                            kdash_client_feed_t feed, char *host, size_t hostsz);

/* Extract `<name>` and `<host>` from `kpidash:services:<name>:<host>`.
 *
 * The 4-segment rule is what makes the family additively evolvable, so it is
 * enforced literally: a key with more or fewer segments is rejected, not
 * trimmed. The `_` sentinel is decoded to an EMPTY `host` — callers test
 * `host[0] == '\0'` for "no host" and never see the sentinel. */
bool kdash_service_key_parse(const char *key, size_t keylen,
                             char *name, size_t namesz,
                             char *host, size_t hostsz);

/* Extract `<zone>` from `kpidash:apttemps:<zone>` (exactly 3 segments). The
 * zone slug from the key is authoritative; the payload's `zone` is a label. */
bool kdash_apttemps_key_parse(const char *key, size_t keylen,
                              char *zone, size_t zonesz);

#endif /* KDASH_KEYS_H */
