/**
 * @file test_freshness.c
 * Both freshness models, at their boundaries — which is the only place a
 * staleness rule is ever wrong.
 */
#include <string.h>

#include "check.h"
#include "kdash/kdash_freshness.h"

int main(void) {
    const long long NOW = 1000000;

    /* ---- age ---- */
    CHECK(kdash_age_s((double)NOW, NOW) == 0, "same instant is age 0");
    CHECK(kdash_age_s((double)(NOW - 30), NOW) == 30, "30 s old");
    /* Timestamps are the WRITER's clock (rules.md), so a stamp from the future
     * is skew, not an error, and reads as fresh rather than as a huge age. */
    CHECK(kdash_age_s((double)(NOW + 120), NOW) == 0, "skew clamps to 0");
    /* The stamp truncates, so a sub-second age can read one second high.
     * Against windows of tens of seconds that is immaterial, and erring old
     * is the safe direction for a staleness rule. */
    CHECK(kdash_age_s((double)NOW - 0.75, NOW) == 1, "fractional ts truncates");
    CHECK(kdash_age_s((double)NOW + 0.75, NOW) == 0, "fractional skew still 0");

    /* ---- ts-owned staleness ---- */
    CHECK(!kdash_ts_stale((double)(NOW - 59), NOW, KDASH_SERVICES_WINDOW_S),
          "59 s inside the 60 s services window");
    CHECK(!kdash_ts_stale((double)(NOW - 60), NOW, KDASH_SERVICES_WINDOW_S),
          "exactly at the window is still fresh");
    CHECK(kdash_ts_stale((double)(NOW - 61), NOW, KDASH_SERVICES_WINDOW_S),
          "one second past the window is stale");
    CHECK(!kdash_ts_stale((double)(NOW - 300), NOW, KDASH_APTTEMPS_WINDOW_S),
          "apttemps window is 300 s");
    CHECK(kdash_ts_stale((double)(NOW - 301), NOW, KDASH_APTTEMPS_WINDOW_S),
          "301 s is stale for apttemps");
    CHECK(!kdash_ts_stale((double)(NOW + 5), NOW, KDASH_SERVICES_WINDOW_S),
          "skew is never stale");

    /* The registry's windows are the contract, so pin them here: a change to
     * either number should have to come through this file. */
    CHECK(KDASH_SERVICES_WINDOW_S == 60, "services window");
    CHECK(KDASH_APTTEMPS_WINDOW_S == 300, "apttemps window");
    CHECK(KDASH_HEALTH_TTL_S == 5 && KDASH_TELEMETRY_TTL_S == 15 &&
              KDASH_DEV_TELEMETRY_TTL_S == 5,
          "expiring-feed TTLs match the registry");

    /* ---- the CD-6 ladder ---- */
    const long long IDLE = 15 * 60, STALE = 40 * 60;
    CHECK(kdash_ladder(0, IDLE, STALE) == KDASH_FRESH, "brand new is fresh");
    CHECK(kdash_ladder(IDLE - 1, IDLE, STALE) == KDASH_FRESH, "just under idle");
    CHECK(kdash_ladder(IDLE, IDLE, STALE) == KDASH_IDLE, "idle starts inclusive");
    CHECK(kdash_ladder(STALE - 1, IDLE, STALE) == KDASH_IDLE, "just under stale");
    CHECK(kdash_ladder(STALE, IDLE, STALE) == KDASH_STALE, "stale starts inclusive");
    CHECK(kdash_ladder(999999, IDLE, STALE) == KDASH_STALE, "very old is stale");

    /* A caller cannot configure an unreachable state: a stale threshold below
     * the idle one collapses onto it rather than hiding KDASH_IDLE. */
    CHECK(kdash_ladder(100, 100, 50) == KDASH_STALE, "inverted thresholds");
    CHECK(kdash_ladder(99, 100, 50) == KDASH_FRESH, "inverted, below both");

    /* ---- labels ---- */
    CHECK(strcmp(kdash_freshness_label(KDASH_FRESH), "fresh") == 0, "fresh label");
    CHECK(strcmp(kdash_freshness_label(KDASH_IDLE), "idle") == 0, "idle label");
    CHECK(strcmp(kdash_freshness_label(KDASH_STALE), "stale") == 0, "stale label");

    return TEST_RESULT();
}
