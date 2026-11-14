/**
 * @file kdash_freshness.c
 * Pure freshness helpers (no Redis, no sockets, no ambient clock).
 */
#include "kdash/kdash_freshness.h"

long long kdash_age_s(double ts, long long now) {
    /* The payload's ts is a float of unix seconds from the writer's clock.
     * Truncating toward zero is right here: sub-second precision cannot
     * matter against windows measured in tens of seconds, and it keeps the
     * skew clamp below exact. */
    long long stamp = (long long)ts;
    long long age = now - stamp;
    return age < 0 ? 0 : age;
}

bool kdash_ts_stale(double ts, long long now, long long window_s) {
    return kdash_age_s(ts, now) > window_s;
}

kdash_freshness_t kdash_ladder(long long age_s, long long idle_s,
                               long long stale_s) {
    if (stale_s < idle_s)
        stale_s = idle_s;
    if (age_s >= stale_s)
        return KDASH_STALE;
    if (age_s >= idle_s)
        return KDASH_IDLE;
    return KDASH_FRESH;
}

bool kdash_cadence_stale(double ts, double expected_refresh_s, long long now,
                         long long grace_s) {
    if (ts <= 0)
        return true;
    long long cadence = expected_refresh_s > 0 ? (long long)expected_refresh_s : 0;
    if (grace_s < 0)
        grace_s = 0;
    return kdash_age_s(ts, now) > cadence + grace_s;
}

const char *kdash_freshness_label(kdash_freshness_t f) {
    switch (f) {
    case KDASH_IDLE:
        return "idle";
    case KDASH_STALE:
        return "stale";
    case KDASH_FRESH:
    default:
        return "fresh";
    }
}
