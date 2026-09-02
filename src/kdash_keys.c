/**
 * @file kdash_keys.c
 * Pure key grammar (no Redis, no sockets, no allocation). Host-testable.
 */
#include "kdash/kdash_keys.h"

#include <string.h>

/* Longest legal token per the contract: 63 chars. */
#define TOKEN_LEN_MAX 63

static bool token_char_ok(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

bool kdash_token_ok(const char *tok, size_t len) {
    if (!tok || len == 0 || len > TOKEN_LEN_MAX)
        return false;
    for (size_t i = 0; i < len; i++)
        if (!token_char_ok(tok[i]))
            return false;
    return true;
}

const char *kdash_client_feed_suffix(kdash_client_feed_t feed) {
    switch (feed) {
    case KDASH_CLIENT_TELEMETRY:
        return ":telemetry";
    case KDASH_CLIENT_DEV_TELEMETRY:
        return ":dev_telemetry";
    case KDASH_CLIENT_HEALTH:
    default:
        return ":health";
    }
}

/* Copy `len` bytes of `src` into `out` (capacity `outsz`) and NUL-terminate.
 * False without touching `out` when it would not fit — this module never
 * truncates a segment and uses the result. */
static bool copy_segment(char *out, size_t outsz, const char *src, size_t len) {
    if (!out || len + 1 > outsz)
        return false;
    memcpy(out, src, len);
    out[len] = '\0';
    return true;
}

bool kdash_client_key(char *out, size_t outsz, const char *host,
                      kdash_client_feed_t feed) {
    if (!out || !host)
        return false;
    size_t hlen = strlen(host);
    if (!kdash_token_ok(host, hlen))
        return false;

    const char *pfx = KDASH_KEY_CLIENT_PFX;
    const char *sfx = kdash_client_feed_suffix(feed);
    size_t plen = strlen(pfx), slen = strlen(sfx);
    if (plen + hlen + slen + 1 > outsz)
        return false;

    memcpy(out, pfx, plen);
    memcpy(out + plen, host, hlen);
    memcpy(out + plen + hlen, sfx, slen);
    out[plen + hlen + slen] = '\0';
    return true;
}

bool kdash_client_key_parse(const char *key, size_t keylen,
                            kdash_client_feed_t feed, char *host,
                            size_t hostsz) {
    if (!key || !host || hostsz == 0)
        return false;

    const char *pfx = KDASH_KEY_CLIENT_PFX;
    const char *sfx = kdash_client_feed_suffix(feed);
    size_t plen = strlen(pfx), slen = strlen(sfx);

    /* Prefix + at least one host char + suffix. */
    if (keylen <= plen + slen)
        return false;
    if (memcmp(key, pfx, plen) != 0)
        return false;
    if (memcmp(key + keylen - slen, sfx, slen) != 0)
        return false;

    const char *h = key + plen;
    size_t hlen = keylen - plen - slen;
    if (!kdash_token_ok(h, hlen))
        return false;
    return copy_segment(host, hostsz, h, hlen);
}

bool kdash_service_key_parse(const char *key, size_t keylen,
                             char *name, size_t namesz,
                             char *host, size_t hostsz) {
    if (!key || !name || !host || namesz == 0 || hostsz == 0)
        return false;

    const char *pfx = KDASH_KEY_SERVICES_PFX;
    size_t plen = strlen(pfx);
    if (keylen <= plen || memcmp(key, pfx, plen) != 0)
        return false;

    /* Exactly one ':' remains, splitting <name> from <host>. More or fewer
     * segments is a different (or malformed) key, and the family's additive
     * evolution depends on us ignoring it rather than reinterpreting it. */
    const char *rest = key + plen;
    size_t restlen = keylen - plen;
    const char *sep = memchr(rest, ':', restlen);
    if (!sep)
        return false;

    size_t nlen = (size_t)(sep - rest);
    const char *h = sep + 1;
    size_t hlen = restlen - nlen - 1;
    if (memchr(h, ':', hlen) != NULL)
        return false; /* a 5th segment */

    if (!kdash_token_ok(rest, nlen))
        return false;

    /* `_` is the documented "no host" sentinel; it decodes to an empty host so
     * no caller has to know the spelling. Any other value is a real host token
     * and must satisfy the contract. */
    bool sentinel = (hlen == 1 && h[0] == '_');
    if (!sentinel && !kdash_token_ok(h, hlen))
        return false;

    if (!copy_segment(name, namesz, rest, nlen))
        return false;
    if (sentinel) {
        host[0] = '\0';
        return true;
    }
    if (!copy_segment(host, hostsz, h, hlen)) {
        name[0] = '\0'; /* all-or-nothing: never hand back half a key */
        return false;
    }
    return true;
}

bool kdash_claude_session_key(char *out, size_t outsz, const char *host,
                              const char *sid) {
    if (!out || !host || !sid)
        return false;
    size_t hlen = strlen(host), slen = strlen(sid);
    if (!kdash_token_ok(host, hlen) || !kdash_token_ok(sid, slen))
        return false;

    const char *pfx = KDASH_KEY_CLAUDE_SESSION_PFX;
    size_t plen = strlen(pfx);
    /* prefix + host + ':' + sid + NUL */
    if (plen + hlen + 1 + slen + 1 > outsz)
        return false;

    memcpy(out, pfx, plen);
    memcpy(out + plen, host, hlen);
    out[plen + hlen] = ':';
    memcpy(out + plen + hlen + 1, sid, slen);
    out[plen + hlen + 1 + slen] = '\0';
    return true;
}

bool kdash_claude_session_key_parse(const char *key, size_t keylen,
                                    char *host, size_t hostsz,
                                    char *sid, size_t sidsz) {
    if (!key || !host || !sid || hostsz == 0 || sidsz == 0)
        return false;

    const char *pfx = KDASH_KEY_CLAUDE_SESSION_PFX;
    size_t plen = strlen(pfx);
    if (keylen <= plen || memcmp(key, pfx, plen) != 0)
        return false;

    const char *rest = key + plen;
    size_t restlen = keylen - plen;
    const char *sep = memchr(rest, ':', restlen);
    if (!sep)
        return false;

    size_t hlen = (size_t)(sep - rest);
    const char *s = sep + 1;
    size_t slen = restlen - hlen - 1;

    /* Both tokens against the contract. That is also what refuses a fifth
     * segment: a ':' inside the sid is not in the token charset, so the sid
     * fails rather than being silently truncated at the second colon. */
    if (!kdash_token_ok(rest, hlen) || !kdash_token_ok(s, slen))
        return false;

    if (!copy_segment(host, hostsz, rest, hlen))
        return false;
    if (!copy_segment(sid, sidsz, s, slen)) {
        host[0] = '\0'; /* all-or-nothing: never hand back half a key */
        return false;
    }
    return true;
}

bool kdash_apttemps_key_parse(const char *key, size_t keylen,
                              char *zone, size_t zonesz) {
    if (!key || !zone || zonesz == 0)
        return false;

    const char *pfx = KDASH_KEY_APTTEMPS_PFX;
    size_t plen = strlen(pfx);
    if (keylen <= plen || memcmp(key, pfx, plen) != 0)
        return false;

    const char *z = key + plen;
    size_t zlen = keylen - plen;
    if (memchr(z, ':', zlen) != NULL)
        return false; /* a 4th segment */
    if (!kdash_token_ok(z, zlen))
        return false;
    return copy_segment(zone, zonesz, z, zlen);
}
