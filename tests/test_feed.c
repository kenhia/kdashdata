/**
 * @file test_feed.c
 * The counted readers' failure path, which until sprint 016 nothing could
 * reach.
 *
 * `kdash_services()`, `kdash_apttemps()` and `kdash_claude_sessions()` all
 * SCAN for keys and then read each one, so they can lose the endpoint half way
 * through a list. Their contract for that (kdash_feed.h) is:
 *
 *   -1 means the read did not complete. `out` is zeroed and `*skipped` is 0,
 *   and NEITHER carries information. A non-negative return is a COMPLETE list.
 *
 * Triggering it against a real Redis means dropping the endpoint BETWEEN a
 * SCAN and a per-key GET, on a server three dashboards read. So these tests
 * swap the unit's I/O seam (src/kdash_feed_internal.h) for a fake that answers
 * from a table and fails the Nth read on demand. No Redis, no socket, no
 * clock -- the same budget as every other suite here.
 *
 * What is actually being pinned is the difference between the two ways a read
 * can come back empty, which is the whole reason the contract exists: an
 * UNAVAIL mid-list is a DROP (-1, nothing), and an ABSENT mid-list is a
 * SKIP (a shorter complete list, counted). A reader that confused them would
 * render missing rows as services that are not running.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "kdash/kdash_feed.h"
#include "kdash_feed_internal.h"

/* ---- the fake endpoint --------------------------------------------------- */

#define FAKE_KEYS_MAX 8

static struct {
    const char *keys[FAKE_KEYS_MAX]; /* what SCAN discovers; NULL-terminated */
    bool scan_ok;                    /* false: the SCAN itself did not finish */
    int fail_at;                     /* 1-based read that reports UNAVAIL     */
    int absent_at;                   /* 1-based read that reports ABSENT      */
    const char *value;               /* what get_string hands back            */
    const char *const *hfields;      /* get_hash's flat field,value,... list  */
    int reads;                       /* per-key reads that arrived            */
} fake;

static void arm(const char *const *keys, int fail_at, int absent_at) {
    memset(&fake, 0, sizeof(fake));
    fake.scan_ok = true;
    fake.fail_at = fail_at;
    fake.absent_at = absent_at;
    for (int i = 0; keys && keys[i] && i < FAKE_KEYS_MAX; i++)
        fake.keys[i] = keys[i];
}

static bool fake_scan(kdash_conn_t *c, const char *match,
                      kdash_scan_visit_fn visit, void *ctx) {
    (void)c;
    (void)match;
    if (!fake.scan_ok)
        return false;
    for (int i = 0; i < FAKE_KEYS_MAX && fake.keys[i]; i++)
        if (!visit(fake.keys[i], strlen(fake.keys[i]), ctx))
            break;
    return true;
}

/* The Nth read, and what it does to it. This one function is the harness. */
static kdash_status_t fake_next(void) {
    fake.reads++;
    if (fake.fail_at == fake.reads)
        return KDASH_UNAVAIL;
    if (fake.absent_at == fake.reads)
        return KDASH_ABSENT;
    return KDASH_OK;
}

static redisReply *bulk(const char *s) {
    redisReply *r = calloc(1, sizeof(*r));
    r->type = REDIS_REPLY_STRING;
    r->str = strdup(s);
    r->len = strlen(s);
    return r;
}

static kdash_status_t fake_get_string(kdash_conn_t *c, const char *key,
                                      redisReply **out) {
    (void)c;
    (void)key;
    *out = NULL;
    kdash_status_t st = fake_next();
    if (st != KDASH_OK)
        return st;
    *out = bulk(fake.value);
    return KDASH_OK;
}

static kdash_status_t fake_get_hash(kdash_conn_t *c, const char *key,
                                    redisReply **out) {
    (void)c;
    (void)key;
    *out = NULL;
    kdash_status_t st = fake_next();
    if (st != KDASH_OK)
        return st;

    size_t n = 0;
    while (fake.hfields[n])
        n++;
    redisReply *r = calloc(1, sizeof(*r));
    r->type = REDIS_REPLY_ARRAY;
    r->elements = n;
    r->element = calloc(n ? n : 1, sizeof(*r->element));
    for (size_t i = 0; i < n; i++)
        r->element[i] = bulk(fake.hfields[i]);
    *out = r;
    return KDASH_OK;
}

/* The seam's fourth member earns itself here: the fake allocates with plain
 * malloc and frees the same way, instead of depending on hiredis' allocator
 * happening to be the one `freeReplyObject` uses. */
static void fake_free_reply(void *reply) {
    redisReply *r = reply;
    if (!r)
        return;
    if (r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < r->elements; i++)
            fake_free_reply(r->element[i]);
        free(r->element);
    } else {
        free(r->str);
    }
    free(r);
}

static const kdash_feed_io_t FAKE_IO = {
    .scan_keys = fake_scan,
    .get_string = fake_get_string,
    .get_hash = fake_get_hash,
    .free_reply = fake_free_reply,
};

/* ---- fixtures ------------------------------------------------------------ */

#define SERVICE_JSON  "{\"state\":\"ok\",\"text\":\"up\",\"ts\":1756000000}"
#define APTTEMPS_JSON "{\"temp_f\":71.5,\"humidity_pct\":40,\"ts\":1756000000}"
#define HEALTH_JSON   "{\"hostname\":\"kai\",\"last_seen_ts\":1756000000}"

static const char *const SERVICE_KEYS[] = {
    "kpidash:services:alpha:kai",
    "kpidash:services:bravo:kai",
    "kpidash:services:charlie:kai",
    NULL,
};
static const char *const ZONE_KEYS[] = {
    "kpidash:apttemps:office",
    "kpidash:apttemps:bedroom",
    "kpidash:apttemps:kitchen",
    NULL,
};
static const char *const SESSION_KEYS[] = {
    "claude:session:kai:aaa",
    "claude:session:kai:bbb",
    "claude:session:kai:ccc",
    NULL,
};
static const char *const SESSION_HASH[] = {
    "status", "working", "ts", "1756000000", NULL,
};

/* Poison rather than zero, so "out is zeroed" is an assertion about what the
 * reader DID and not about what the buffer happened to hold. */
#define POISON 0xA5

static bool all_zero(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++)
        if (b[i])
            return false;
    return true;
}

int main(void) {
    /* The seam defaults to the real thing with nothing called, which is what
     * keeps a consumer that never hears of it on hiredis. */
    CHECK(kdash_feed_io() == kdash_feed_io_real(),
          "the real I/O is installed at load, with no initialisation call");

    kdash_conn_t *c = kdash_conn_new(NULL); /* allocates; never connects */
    CHECK(c != NULL, "handle allocated");
    if (!c)
        return TEST_RESULT();

    kdash_feed_set_io(&FAKE_IO);
    CHECK(kdash_feed_io() == &FAKE_IO, "the fake is installed");

    /* ---- kdash_services ---- */

    kdash_service_t svcs[4];
    int skipped;

    fake.value = SERVICE_JSON;
    arm(SERVICE_KEYS, 0, 0);
    fake.value = SERVICE_JSON;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == 3, "three keys, three rows");
    CHECK(skipped == 0, "nothing skipped on a clean pass");
    CHECK(strcmp(svcs[0].name, "alpha") == 0 && strcmp(svcs[0].host, "kai") == 0,
          "identity comes from the key");
    CHECK(svcs[0].state == KDASH_SVC_OK, "payload parsed");

    /* The whole point: the endpoint goes away on the LAST of three reads. */
    arm(SERVICE_KEYS, 3, 0);
    fake.value = SERVICE_JSON;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == -1,
          "a mid-list drop is -1, never the two rows already gathered");
    CHECK(skipped == 0, "*skipped carries no information after a drop");
    CHECK(all_zero(svcs, sizeof(svcs)),
          "out is zeroed across the caller's whole buffer, not just the rows read");

    /* ...and on the FIRST, where there is nothing gathered to hand back. */
    arm(SERVICE_KEYS, 1, 0);
    fake.value = SERVICE_JSON;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == -1, "a drop on the first read is -1");
    CHECK(skipped == 0 && all_zero(svcs, sizeof(svcs)), "and leaves the same state");

    /* The control that gives the -1 its meaning: an ABSENT key mid-list is a
     * key that raced with an expiry, which is a SKIP and a complete list. */
    arm(SERVICE_KEYS, 0, 2);
    fake.value = SERVICE_JSON;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == 2,
          "an absent key mid-list shortens the list; it does not drop it");
    CHECK(skipped == 1, "and is counted");

    /* The SCAN itself failing is the other half of the same contract. */
    arm(SERVICE_KEYS, 0, 0);
    fake.value = SERVICE_JSON;
    fake.scan_ok = false;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == -1, "a failed SCAN is -1");
    CHECK(skipped == 0, "*skipped is 0 after a failed SCAN");
    CHECK(all_zero(svcs, sizeof(svcs)), "and out is zeroed (repaired in sprint 016)");

    /* A grammar rejection never reaches a GET, so it cannot be a drop. */
    static const char *const MIXED_KEYS[] = {
        "kpidash:services:alpha:kai",
        "kpidash:services:too:many:segments",
        "kpidash:services:bravo:kai",
        NULL,
    };
    arm(MIXED_KEYS, 0, 0);
    fake.value = SERVICE_JSON;
    memset(svcs, POISON, sizeof(svcs));
    skipped = -7;
    CHECK(kdash_services(c, svcs, 4, &skipped) == 2, "the bad key is skipped at the choke point");
    CHECK(skipped == 1 && fake.reads == 2, "and never reached a GET");

    /* ---- kdash_apttemps ---- */

    kdash_apttemps_t zones[4];
    arm(ZONE_KEYS, 0, 0);
    fake.value = APTTEMPS_JSON;
    memset(zones, POISON, sizeof(zones));
    skipped = -7;
    CHECK(kdash_apttemps(c, zones, 4, &skipped) == 3, "three zones");
    CHECK(skipped == 0 && strcmp(zones[0].zone, "office") == 0, "zone from the key");

    arm(ZONE_KEYS, 2, 0);
    fake.value = APTTEMPS_JSON;
    memset(zones, POISON, sizeof(zones));
    skipped = -7;
    CHECK(kdash_apttemps(c, zones, 4, &skipped) == -1, "apttemps drops the same way");
    CHECK(skipped == 0 && all_zero(zones, sizeof(zones)), "and zeroes the same way");

    arm(ZONE_KEYS, 0, 0);
    fake.value = APTTEMPS_JSON;
    fake.scan_ok = false;
    memset(zones, POISON, sizeof(zones));
    skipped = -7;
    CHECK(kdash_apttemps(c, zones, 4, &skipped) == -1, "a failed SCAN is -1");
    CHECK(skipped == 0 && all_zero(zones, sizeof(zones)), "with out zeroed");

    /* ---- kdash_claude_sessions (HGETALL, not GET) ---- */

    kdash_claude_session_t sessions[4];
    arm(SESSION_KEYS, 0, 0);
    fake.hfields = SESSION_HASH;
    memset(sessions, POISON, sizeof(sessions));
    skipped = -7;
    CHECK(kdash_claude_sessions(c, sessions, 4, &skipped) == 3, "three sessions");
    CHECK(skipped == 0, "nothing skipped");
    CHECK(strcmp(sessions[0].host, "kai") == 0 && strcmp(sessions[0].sid, "aaa") == 0,
          "host and sid from the key");
    CHECK(sessions[0].status == KDASH_CLAUDE_WORKING, "hash parsed");

    arm(SESSION_KEYS, 2, 0);
    fake.hfields = SESSION_HASH;
    memset(sessions, POISON, sizeof(sessions));
    skipped = -7;
    CHECK(kdash_claude_sessions(c, sessions, 4, &skipped) == -1,
          "a mid-list HGETALL drop is -1");
    CHECK(skipped == 0, "*skipped carries nothing");
    CHECK(all_zero(sessions, sizeof(sessions)), "out zeroed");

    /* This reader parses each key straight into `out` as the SCAN yields it,
     * so a failed SCAN is the one that would otherwise leave rows carrying a
     * host and a sid and no payload. */
    arm(SESSION_KEYS, 0, 0);
    fake.hfields = SESSION_HASH;
    fake.scan_ok = false;
    memset(sessions, POISON, sizeof(sessions));
    skipped = -7;
    CHECK(kdash_claude_sessions(c, sessions, 4, &skipped) == -1, "a failed SCAN is -1");
    CHECK(skipped == 0 && all_zero(sessions, sizeof(sessions)),
          "and leaves no half-built row behind (repaired in sprint 016)");

    /* ---- the single-key readers ---- */
    /*
     * The counted rule is vacuous for these, but the distinction underneath it
     * is not: a dead socket must surface as KDASH_UNAVAIL and never as
     * KDASH_ABSENT, because to a panel those mean opposite things.
     */
    kdash_health_t health;
    arm(NULL, 0, 0);
    fake.value = HEALTH_JSON;
    memset(&health, POISON, sizeof(health));
    CHECK(kdash_health(c, "kai", &health) == KDASH_OK, "health reads");
    CHECK(strcmp(health.hostname, "kai") == 0, "and parses");

    arm(NULL, 1, 0);
    fake.value = HEALTH_JSON;
    memset(&health, POISON, sizeof(health));
    CHECK(kdash_health(c, "kai", &health) == KDASH_UNAVAIL,
          "an unreachable endpoint is UNAVAIL, not ABSENT");
    CHECK(all_zero(&health, sizeof(health)), "and out is zeroed");

    arm(NULL, 0, 1);
    fake.value = HEALTH_JSON;
    memset(&health, POISON, sizeof(health));
    CHECK(kdash_health(c, "kai", &health) == KDASH_ABSENT,
          "a missing key is ABSENT, which is a different thing to say");

    /* ---- putting it back ---- */

    kdash_feed_set_io(NULL);
    CHECK(kdash_feed_io() == kdash_feed_io_real(), "NULL restores the real I/O");

    kdash_conn_free(c);
    return TEST_RESULT();
}
