/**
 * @file test_keys.c
 * The key-grammar choke point: what it accepts, what it refuses, and — just
 * as important — that a refusal never leaves a half-filled buffer behind for
 * a caller to use by accident.
 */
#include <stdio.h>
#include <string.h>

#include "check.h"
#include "kdash/kdash_keys.h"

/* A key must be accepted and yield exactly this host. */
static void client_ok(const char *key, kdash_client_feed_t feed,
                      const char *want) {
    char host[KDASH_TOKEN_MAX];
    bool got = kdash_client_key_parse(key, strlen(key), feed, host, sizeof(host));
    CHECK(got, "expected accept: %s", key);
    if (got)
        CHECK(strcmp(host, want) == 0, "%s -> \"%s\", want \"%s\"", key, host, want);
}

/* A key must be refused, and `host` must come back untouched. */
static void client_reject(const char *key, kdash_client_feed_t feed) {
    char host[KDASH_TOKEN_MAX];
    memcpy(host, "SENTINEL", 9);
    bool got = kdash_client_key_parse(key, strlen(key), feed, host, sizeof(host));
    CHECK(!got, "expected reject: %s", key);
    CHECK(memcmp(host, "SENTINEL", 9) == 0, "%s: rejected but clobbered out", key);
}

static void service_ok(const char *key, const char *want_name,
                       const char *want_host) {
    char name[KDASH_NAME_MAX], host[KDASH_TOKEN_MAX];
    bool got = kdash_service_key_parse(key, strlen(key), name, sizeof(name),
                                       host, sizeof(host));
    CHECK(got, "expected accept: %s", key);
    if (got) {
        CHECK(strcmp(name, want_name) == 0, "%s -> name \"%s\", want \"%s\"", key,
              name, want_name);
        CHECK(strcmp(host, want_host) == 0, "%s -> host \"%s\", want \"%s\"", key,
              host, want_host);
    }
}

static void service_reject(const char *key) {
    char name[KDASH_NAME_MAX], host[KDASH_TOKEN_MAX];
    memcpy(name, "SENTINEL", 9);
    memcpy(host, "SENTINEL", 9);
    bool got = kdash_service_key_parse(key, strlen(key), name, sizeof(name),
                                       host, sizeof(host));
    CHECK(!got, "expected reject: %s", key);
}

static void claude_ok(const char *key, const char *want_host,
                      const char *want_sid) {
    char host[KDASH_TOKEN_MAX], sid[KDASH_TOKEN_MAX];
    bool got = kdash_claude_session_key_parse(key, strlen(key), host,
                                              sizeof(host), sid, sizeof(sid));
    CHECK(got, "expected accept: %s", key);
    if (got) {
        CHECK(strcmp(host, want_host) == 0, "%s -> host \"%s\", want \"%s\"", key,
              host, want_host);
        CHECK(strcmp(sid, want_sid) == 0, "%s -> sid \"%s\", want \"%s\"", key, sid,
              want_sid);
    }
}

/* A key must be refused, and NEITHER output may be touched — a half-filled
 * host is exactly what a caller would go on to build a key from. */
static void claude_reject(const char *key) {
    char host[KDASH_TOKEN_MAX], sid[KDASH_TOKEN_MAX];
    memcpy(host, "SENTINEL", 9);
    memcpy(sid, "SENTINEL", 9);
    bool got = kdash_claude_session_key_parse(key, strlen(key), host,
                                              sizeof(host), sid, sizeof(sid));
    CHECK(!got, "expected reject: %s", key);
    CHECK(memcmp(host, "SENTINEL", 9) == 0, "%s: rejected but clobbered host", key);
    CHECK(memcmp(sid, "SENTINEL", 9) == 0, "%s: rejected but clobbered sid", key);
}

int main(void) {
    /* ---- the token contract ---- */
    CHECK(kdash_token_ok("kai", 3), "plain host");
    CHECK(kdash_token_ok("a", 1), "one char");
    CHECK(kdash_token_ok("rpi-5_3.local", 13), "full charset");
    CHECK(!kdash_token_ok("", 0), "empty token");
    CHECK(!kdash_token_ok(NULL, 3), "NULL token");
    CHECK(!kdash_token_ok("has space", 9), "space is not in the charset");
    CHECK(!kdash_token_ok("colon:here", 10), "a colon would forge a segment");
    CHECK(!kdash_token_ok("star*", 5), "glob metacharacter");
    {
        char long63[64], long64[65];
        memset(long63, 'a', 63);
        memset(long64, 'a', 64);
        CHECK(kdash_token_ok(long63, 63), "63 chars is the limit and is legal");
        CHECK(!kdash_token_ok(long64, 64), "64 chars is over the limit");
    }

    /* ---- client keys: build ---- */
    {
        char key[KDASH_KEY_MAX];
        CHECK(kdash_client_key(key, sizeof(key), "kai", KDASH_CLIENT_HEALTH),
              "build health key");
        CHECK(strcmp(key, "kpidash:client:kai:health") == 0, "built \"%s\"", key);
        CHECK(kdash_client_key(key, sizeof(key), "kai", KDASH_CLIENT_TELEMETRY),
              "build telemetry key");
        CHECK(strcmp(key, "kpidash:client:kai:telemetry") == 0, "built \"%s\"", key);
        CHECK(kdash_client_key(key, sizeof(key), "kai", KDASH_CLIENT_DEV_TELEMETRY),
              "build dev_telemetry key");
        CHECK(strcmp(key, "kpidash:client:kai:dev_telemetry") == 0, "built \"%s\"",
              key);

        /* Construction is a choke point too: a host that failed the contract
         * must never become a key, however it reached us. */
        CHECK(!kdash_client_key(key, sizeof(key), "bad host", KDASH_CLIENT_HEALTH),
              "space in host must not build a key");
        CHECK(!kdash_client_key(key, sizeof(key), "a:b", KDASH_CLIENT_HEALTH),
              "colon in host must not build a key");
        CHECK(!kdash_client_key(key, sizeof(key), "", KDASH_CLIENT_HEALTH),
              "empty host must not build a key");
        char tiny[8];
        CHECK(!kdash_client_key(tiny, sizeof(tiny), "kai", KDASH_CLIENT_HEALTH),
              "buffer too small must fail, not truncate");
    }

    /* ---- client keys: parse ---- */
    client_ok("kpidash:client:kai:health", KDASH_CLIENT_HEALTH, "kai");
    client_ok("kpidash:client:a:telemetry", KDASH_CLIENT_TELEMETRY, "a");
    client_ok("kpidash:client:rpi-5_3.local:dev_telemetry",
              KDASH_CLIENT_DEV_TELEMETRY, "rpi-5_3.local");

    /* Anchoring: the suffix is part of the grammar, so the feeds do not
     * cross-match even though their prefixes are identical. */
    client_reject("kpidash:client:kai:telemetry", KDASH_CLIENT_HEALTH);
    client_reject("kpidash:client:kai:dev_telemetry", KDASH_CLIENT_TELEMETRY);
    client_reject("kpidash:client::health", KDASH_CLIENT_HEALTH);
    client_reject("kpidash:client:kai:health:extra", KDASH_CLIENT_HEALTH);
    client_reject("kpidash:client:kai", KDASH_CLIENT_HEALTH);
    client_reject(":health", KDASH_CLIENT_HEALTH);
    client_reject("kpidashXclient:kai:health", KDASH_CLIENT_HEALTH);
    client_reject("kpidash:client:has space:health", KDASH_CLIENT_HEALTH);
    /* A host that is itself two segments would silently become a different
     * host if we split on the last colon instead of anchoring. */
    client_reject("kpidash:client:a:b:health", KDASH_CLIENT_HEALTH);

    /* ---- service keys: exactly 4 segments, `_` sentinel ---- */
    service_ok("kpidash:services:korg:kubs0", "korg", "kubs0");
    service_ok("kpidash:services:backup:_", "backup", ""); /* sentinel -> "" */
    service_ok("kpidash:services:a:b", "a", "b");

    service_reject("kpidash:services:korg");          /* 3 segments */
    service_reject("kpidash:services:korg:kubs0:x");  /* 5 segments */
    service_reject("kpidash:services::kubs0");        /* empty name */
    service_reject("kpidash:services:korg:");         /* empty host */
    service_reject("kpidash:services:korg:has space");
    service_reject("kpidash:client:kai:health");      /* another family */
    {
        /* An overflowing host must not leave the name half-written. */
        char name[KDASH_NAME_MAX], host[4];
        strcpy(name, "SENTINEL");
        bool got = kdash_service_key_parse("kpidash:services:korg:kubs0", 27,
                                           name, sizeof(name), host, sizeof(host));
        CHECK(!got, "host buffer too small must fail");
        CHECK(name[0] == '\0', "failed parse must not leave a usable name");
    }

    /* ---- apttemps keys: exactly 3 segments ---- */
    {
        char zone[KDASH_ZONE_MAX];
        CHECK(kdash_apttemps_key_parse("kpidash:apttemps:office", 23, zone,
                                       sizeof(zone)),
              "accept a zone");
        CHECK(strcmp(zone, "office") == 0, "zone \"%s\"", zone);
        CHECK(!kdash_apttemps_key_parse("kpidash:apttemps:", 17, zone, sizeof(zone)),
              "empty zone");
        CHECK(!kdash_apttemps_key_parse("kpidash:apttemps:a:b", 20, zone,
                                        sizeof(zone)),
              "4 segments is not this family");
        CHECK(!kdash_apttemps_key_parse("kpidash:apttemp:office", 22, zone,
                                        sizeof(zone)),
              "corrupted prefix");
    }

    /* Keys need not be NUL-terminated — SCAN hands back counted strings. */
    {
        const char buf[] = "kpidash:client:kai:healthGARBAGE";
        char host[KDASH_TOKEN_MAX];
        CHECK(kdash_client_key_parse(buf, 25, KDASH_CLIENT_HEALTH, host,
                                     sizeof(host)),
              "length-bounded parse ignores trailing bytes");
        CHECK(strcmp(host, "kai") == 0, "host \"%s\"", host);
    }

    /* ---- panel keys: construction only, and that is the point ---- */
    {
        char key[KDASH_KEY_MAX];
        CHECK(kdash_panel_key(key, sizeof(key), "kstudio"), "build a panel key");
        CHECK(strcmp(key, "kdash:panel:kstudio") == 0, "built \"%s\"", key);

        /* This family is never discovered, so construction is the ONLY choke
         * point it has — a bad host here would reach Redis unchallenged. */
        CHECK(!kdash_panel_key(key, sizeof(key), "kai:evil"),
              "a host carrying ':' must not construct a key");
        CHECK(!kdash_panel_key(key, sizeof(key), "bad host"),
              "space in host must not construct a key");
        CHECK(!kdash_panel_key(key, sizeof(key), ""), "empty host");
        CHECK(!kdash_panel_key(key, sizeof(key), "kstudio/../kai"),
              "path-ish host must not construct a key");

        char tiny[8];
        memcpy(tiny, "SENTINEL", 8);
        CHECK(!kdash_panel_key(tiny, sizeof(tiny), "kstudio"),
              "buffer too small must fail, not truncate");
        CHECK(memcmp(tiny, "SENTINEL", 8) == 0, "refusal must not clobber out");

        /* KDASH_KEY_MAX sizes the readers' key buffers, and this family shares
         * it rather than getting a constant of its own — so the widest legal
         * panel key has to fit. */
        {
            char tok[KDASH_TOKEN_MAX];
            memset(tok, 'x', KDASH_TOKEN_MAX - 1);
            tok[KDASH_TOKEN_MAX - 1] = '\0';
            CHECK(kdash_panel_key(key, sizeof(key), tok),
                  "a 63-char host must fit KDASH_KEY_MAX");
        }
    }

    /* ---- claude:session:<host>:<sid> ---- */
    {
        claude_ok("claude:session:kai:abc123", "kai", "abc123");
        claude_ok("claude:session:rpi-5_3.local:0f3c.d-9", "rpi-5_3.local",
                  "0f3c.d-9");
        claude_ok("claude:session:a:b", "a", "b");

        claude_reject("claude:session:");           /* prefix only          */
        claude_reject("claude:session:kai");        /* no sid segment       */
        claude_reject("claude:session::abc123");    /* empty host           */
        claude_reject("claude:session:kai:");       /* empty sid            */
        claude_reject("claude:session:kai:a:b");    /* a 5th segment        */
        claude_reject("claude:sessions:kai:abc");   /* corrupted prefix     */
        claude_reject("kpidash:session:kai:abc");   /* another family       */
        claude_reject("claude:limits");             /* not this family      */
        claude_reject("claude:session:k ai:abc");   /* space is not a token */
        claude_reject("claude:session:kai:ab/c");   /* nor is a slash       */

        /* 64 chars in either token is one over the contract. */
        {
            char big[160];
            char tok[65];
            memset(tok, 'x', 64);
            tok[64] = '\0';
            snprintf(big, sizeof(big), "claude:session:%s:abc", tok);
            claude_reject(big);
            snprintf(big, sizeof(big), "claude:session:kai:%s", tok);
            claude_reject(big);
        }

        /* SCAN hands back counted strings, not NUL-terminated ones. */
        {
            const char buf[] = "claude:session:kai:abc123GARBAGE";
            char host[KDASH_TOKEN_MAX], sid[KDASH_TOKEN_MAX];
            CHECK(kdash_claude_session_key_parse(buf, 25, host, sizeof(host), sid,
                                                 sizeof(sid)),
                  "length-bounded parse ignores trailing bytes");
            CHECK(strcmp(host, "kai") == 0 && strcmp(sid, "abc123") == 0,
                  "host \"%s\" sid \"%s\"", host, sid);
        }

        /* An output buffer too small refuses rather than truncating. */
        {
            char host[4], sid[KDASH_TOKEN_MAX];
            CHECK(!kdash_claude_session_key_parse("claude:session:kubs0:abc", 24,
                                                  host, sizeof(host), sid,
                                                  sizeof(sid)),
                  "host buffer too small must refuse");
        }
    }

    /* ---- claude:session key construction ---- */
    {
        char key[KDASH_CLAUDE_KEY_MAX];
        CHECK(kdash_claude_session_key(key, sizeof(key), "kai", "abc123"),
              "build a session key");
        CHECK(strcmp(key, "claude:session:kai:abc123") == 0, "built \"%s\"", key);

        /* Construction is a choke point too: the segments may have come back
         * off a SCAN or out of a config file. */
        CHECK(!kdash_claude_session_key(key, sizeof(key), "kai:evil", "abc"),
              "a host carrying ':' must not construct a key");
        CHECK(!kdash_claude_session_key(key, sizeof(key), "", "abc"),
              "empty host");
        CHECK(!kdash_claude_session_key(key, sizeof(key), "kai", ""),
              "empty sid");

        char small[16];
        CHECK(!kdash_claude_session_key(small, sizeof(small), "kai", "abc123"),
              "buffer too small must refuse");

        /* KDASH_CLAUDE_KEY_MAX must hold the widest legal key. */
        {
            char tok[KDASH_TOKEN_MAX];
            memset(tok, 'x', KDASH_TOKEN_MAX - 1);
            tok[KDASH_TOKEN_MAX - 1] = '\0';
            CHECK(kdash_claude_session_key(key, sizeof(key), tok, tok),
                  "two 63-char tokens must fit KDASH_CLAUDE_KEY_MAX");
            char host[KDASH_TOKEN_MAX], sid[KDASH_TOKEN_MAX];
            CHECK(kdash_claude_session_key_parse(key, strlen(key), host,
                                                 sizeof(host), sid, sizeof(sid)),
                  "and must parse back");
        }
    }

    return TEST_RESULT();
}
