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
 *   kdash:panel:<host>                          exactly 3 segments
 *   claude:session:<host>:<sid>                 4 segments, last is opaque
 *   claude:limits                               one shared key
 *   claude:recent                               one shared key
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

/* The kdash namespace's control feed (registry.md): one key per panel. */
#define KDASH_KEY_PANEL_PFX "kdash:panel:"

/* The claude family (registry.md). Two of the three are single fixed keys, so
 * they are literals rather than grammars; only the session family is parsed. */
#define KDASH_KEY_CLAUDE_SESSION_PFX "claude:session:"
#define KDASH_KEY_CLAUDE_LIMITS      "claude:limits"
#define KDASH_KEY_CLAUDE_RECENT      "claude:recent"

/* The `<host>` sentinel meaning "this service has no host" (registry.md). */
#define KDASH_HOST_SENTINEL "_"

/* Longest key the kpidash families can produce, plus NUL. The client family
 * is the widest: prefix + 63-char host + ":dev_telemetry". */
#define KDASH_KEY_MAX 128

/* Longest claude:session key, plus NUL: the prefix plus two full-length tokens
 * and the ':' between them. Deliberately NOT folded into KDASH_KEY_MAX — that
 * constant sizes the kpidash readers' SCAN key buffers, and widening it would
 * grow their stack frames for a family they never see. */
#define KDASH_CLAUDE_KEY_MAX \
    (sizeof(KDASH_KEY_CLAUDE_SESSION_PFX) - 1 + 2 * KDASH_TOKEN_MAX)

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

/* Build `kdash:panel:<host>` (exactly 3 segments) into `out`. `host` is
 * revalidated against the token contract first, exactly as kdash_client_key()
 * does it: the name may have come from config or from a hostname lookup, and
 * neither is a contract.
 *
 * There is deliberately no matching parse. Every other family here is
 * DISCOVERED — a SCAN or a member set hands a reader keys it did not choose,
 * so parsing is where the choke point has to sit. This one is not discovered:
 * a panel builds the key with its own name on it, and construction is the only
 * place a bad token could get in. Add the parse the day something enumerates
 * panels, not before. */
bool kdash_panel_key(char *out, size_t outsz, const char *host);

/* Build `claude:session:<host>:<sid>`. Both tokens are revalidated first: a
 * reader constructs this key from segments it read back off a SCAN, so
 * construction is a choke point exactly like kdash_client_key(). */
bool kdash_claude_session_key(char *out, size_t outsz, const char *host,
                              const char *sid);

/* Extract `<host>` and `<sid>` from `claude:session:<host>:<sid>`.
 *
 * The shape differs from the kpidash client family: four segments whose LAST
 * one is an opaque session id rather than a fixed feed suffix, so there is no
 * anchor to match at the end and this gets its own entry point rather than
 * bending kdash_client_key_parse's mould. Both tokens must satisfy the token
 * contract — which is also what rejects an embedded ':' in the sid, i.e. a
 * fifth segment. All-or-nothing: a refusal leaves both outputs untouched. */
bool kdash_claude_session_key_parse(const char *key, size_t keylen,
                                    char *host, size_t hostsz,
                                    char *sid, size_t sidsz);

#endif /* KDASH_KEYS_H */
