/**
 * @file kdash_endpoint.h
 * Where the central Redis lives — CD-4's endpoint discovery, implemented once.
 *
 * khlenv is the homelab's config resolver: ask it for a key and it walks
 * `KEY.<host>.<app>` -> `KEY.<host>` -> `KEY`, deriving `<host>` from the
 * caller's address. Its protocol is deliberately dumb — one plain-HTTP GET:
 *
 *   GET <endpoint>/v1/resolve?app=<app>&key=<KEY>
 *     200  the value, in the body
 *     204  an EXPLICIT null: "deliberately no endpoint". Not a miss, and not
 *          a reason to fall back to anything.
 *     404  a miss; the body names the walk it tried
 *
 * `<endpoint>` is the one line in /etc/khlenv/endpoint (e.g.
 * `http://kubs0.example.ts.net:7770`). There is no C khlenv client — the
 * Python and Rust clients are the only ones — so this module speaks the
 * protocol directly over a bounded-timeout socket rather than taking a
 * dependency on libcurl for one unencrypted GET. The response parsing is
 * pure and host-tested; only the socket half needs a live service.
 *
 * khlenv NEVER holds secrets (CD-2). The Redis password comes from the
 * REDISCLI_AUTH environment variable and nowhere else.
 */
#ifndef KDASH_ENDPOINT_H
#define KDASH_ENDPOINT_H

#include <stdbool.h>
#include <stddef.h>

/* The file naming the khlenv service; override with $KDASH_KHLENV_ENDPOINT
 * (which holds the endpoint URL itself, not a path) for tests and for hosts
 * that have no /etc/khlenv. */
#define KDASH_KHLENV_ENDPOINT_FILE "/etc/khlenv/endpoint"
#define KDASH_KHLENV_ENDPOINT_ENV  "KDASH_KHLENV_ENDPOINT"

/* CD-4: the stem every new consumer and publisher resolves. */
#define KDASH_CENTRAL_STEM "KDASH_CENTRAL_REDIS"
/* The legacy alias for the same endpoint, kept until its publishers migrate
 * (CD-3/CD-4). Tried only when the new stem MISSES — never when it answers
 * with an explicit null. */
#define KDASH_CENTRAL_STEM_LEGACY "KPIDASH_REDIS"

/* Where the central Redis has lived since kpidash 001. Used only when khlenv
 * itself cannot be reached and no override was given: falling back to it can
 * never be worse than having no khlenv at all (the kpidash-client rule). */
#define KDASH_CENTRAL_DEFAULT "rpi53:6379"

/* CD-7: the `claude:*` family's own home. It answers `rpi53:6379` today —
 * the SAME endpoint the central stem answers — and that is exactly why it must
 * never be resolved through the central stem as a shortcut: the family keeps
 * its own address so it can move again with one khlenv store edit and no
 * publisher or dashboard host being touched. A reader that quietly used the
 * central stem would pass every test today and be wrong on the day of the
 * move, which is the failure the two stems exist to prevent. */
#define KDASH_CLAUDE_STEM "KDASH_CLAUDE_REDIS"

#define KDASH_REDIS_PORT_DEFAULT 6379
#define KDASH_AUTH_ENV "REDISCLI_AUTH"

/* Longest value khlenv will hand back for an endpoint, plus NUL. */
#define KDASH_ENDPOINT_MAX 256

typedef enum {
    KDASH_KHLENV_OK = 0,  /* 200 — value copied out                        */
    KDASH_KHLENV_NULL,    /* 204 — deliberate "no endpoint"; do not fall back */
    KDASH_KHLENV_MISS,    /* 404 — no value at any stem                    */
    KDASH_KHLENV_UNAVAIL, /* unreachable, timed out, or an unusable reply   */
} kdash_khlenv_status_t;

typedef enum {
    KDASH_EP_OK = 0,   /* host/port resolved and usable                     */
    KDASH_EP_NONE,     /* khlenv holds an explicit null — nothing to connect to */
    KDASH_EP_INVALID,  /* a value was found but is not a usable host[:port] */
    /* Nothing answered anywhere and this stem has no compiled-in fallback:
     * khlenv was unreachable, or the store holds no value at any level. Only
     * a stem WITHOUT a fallback can return this — see kdash_stem_t. */
    KDASH_EP_UNRESOLVED,
} kdash_endpoint_status_t;

/**
 * One stem and the walk that belongs to it.
 *
 * The walk itself is CD-4's and never varies: environment override, then
 * khlenv's stem, then its legacy alias on a MISS only, then the compiled-in
 * fallback when khlenv could not be reached at all. What varies per family is
 * whether the last two steps exist — so they are DATA here rather than
 * branches, and this struct is the C spelling of the `Stem` the Rust and
 * Python publishers already carry (endpoint.rs, endpoint.py). Three
 * implementations of one walk agreeing is worth more than three that each
 * hardcode their own.
 *
 * `fallback` is named for what it does rather than mirroring the wrappers'
 * `default`, which is a keyword here.
 */
typedef struct {
    /* The environment variable AND the khlenv key — one name, because an
     * override that did not spell the stem would be a second thing to know. */
    const char *key;
    /* Tried only on a MISS. NULL when this family has no alias. */
    const char *legacy;
    /* `host[:port]` to stand in when khlenv itself cannot be reached. NULL
     * when this family would rather resolve nothing than guess. */
    const char *fallback;
} kdash_stem_t;

/* The central Redis: legacy alias, and the historical default. */
extern const kdash_stem_t KDASH_STEM_CENTRAL;

/* The claude family's home — no alias, and **no fallback on purpose.**
 *
 * CD-4's fallback exists because the central Redis has always been at
 * rpi53:6379, so for a reader a stale-but-right guess beats nothing. The
 * claude stem is the one that MOVES (CD-7 moved it once already), so a
 * compiled-in guess is wrong on one side of a cutover or the other — and a
 * dashboard rendering a confident panel out of the Redis nobody is writing to
 * is a worse answer than one rendering "unavailable". A stem nobody has
 * decided a fallback for does not get one invented at the call site. */
extern const kdash_stem_t KDASH_STEM_CLAUDE;

/* ---- pure (host-testable) ---- */

/* Split `host` or `host:port` into parts. Returns false on an empty string, an
 * empty host, a non-numeric port, or a port outside 1..65535. `defport` is used
 * when no `:port` is present. An IPv6 literal is not accepted — the homelab
 * addresses Redis by name or by IPv4, and silently mis-splitting `::1` would be
 * worse than refusing it. */
bool kdash_parse_hostport(const char *value, int defport, char *host,
                          size_t hostsz, int *port);

/* Parse the contents of /etc/khlenv/endpoint (`http://host:port`, with or
 * without a trailing newline or path) into host/port. `http://` is optional and
 * the only scheme accepted; the default port is 80. */
bool kdash_khlenv_parse_endpoint(const char *text, size_t len, char *host,
                                 size_t hostsz, int *port);

/* Build the request line + headers for one resolve. `app` and `key` are
 * validated against the token contract rather than percent-escaped — the
 * legal charset has no reserved characters, so refusing the rest is both
 * simpler and a tighter choke point than escaping it. */
bool kdash_khlenv_build_request(char *out, size_t outsz, const char *host,
                                int port, const char *app, const char *key);

/* Parse a raw HTTP/1.x response. Returns the status classification and, for
 * 200, copies the trimmed body into `value`. A 200 with an empty body is
 * KDASH_KHLENV_UNAVAIL — a value that is not there is not a value. */
kdash_khlenv_status_t kdash_khlenv_parse_response(const char *buf, size_t len,
                                                  char *value, size_t valuesz);

/* ---- I/O ---- */

/* One live resolve. Bounded: a connect timeout and a total read deadline, so
 * a dead khlenv costs a fixed pause, never a hang (CD-6). */
kdash_khlenv_status_t kdash_khlenv_resolve(const char *app, const char *key,
                                           char *value, size_t valuesz);

/**
 * Resolve the central Redis endpoint for `app`, in CD-4 order:
 *
 *   1. `$KDASH_CENTRAL_REDIS` in the environment — an explicit override wins
 *      outright, so an install can always pin an endpoint.
 *   2. khlenv `KDASH_CENTRAL_REDIS`. An explicit null here returns
 *      KDASH_EP_NONE and stops: "deliberately no endpoint" is an answer.
 *   3. khlenv `KPIDASH_REDIS`, the legacy alias, on a MISS only.
 *   4. KDASH_CENTRAL_DEFAULT, when khlenv could not be reached at all.
 *
 * Callers resolve on EVERY connect — that is what makes a moved Redis
 * propagate within one reconnect interval instead of needing a config edit on
 * every consumer.
 */
kdash_endpoint_status_t kdash_resolve_central(const char *app, char *host,
                                              size_t hostsz, int *port);

/**
 * Resolve any stem, in CD-4 order:
 *
 *   1. `$<stem->key>` in the environment — an explicit override wins
 *      outright, so an install can always pin an endpoint.
 *   2. khlenv `<stem->key>`. An explicit null here returns KDASH_EP_NONE and
 *      stops: "deliberately no endpoint" is an answer.
 *   3. khlenv `<stem->legacy>`, on a MISS only — and never after an
 *      unreachable khlenv, because the alias lives in the same store on the
 *      same service, so asking twice is a second timeout, not a second chance.
 *   4. `<stem->fallback>`, when there is one.
 *
 * Returns KDASH_EP_UNRESOLVED when the walk ran out and the stem has no
 * fallback. kdash_resolve_central() is this with KDASH_STEM_CENTRAL, kept as
 * its own name because it is what every kpidash reader already calls.
 */
kdash_endpoint_status_t kdash_resolve_endpoint(const kdash_stem_t *stem,
                                               const char *app, char *host,
                                               size_t hostsz, int *port);

#endif /* KDASH_ENDPOINT_H */
