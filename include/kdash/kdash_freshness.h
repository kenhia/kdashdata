/**
 * @file kdash_freshness.h
 * Pure freshness/staleness helpers for both feed patterns. No Redis, no
 * sockets, no clock of its own — every function takes `now` so the ladder is
 * host-testable at its boundaries.
 *
 * contracts/rules.md defines two latest-value patterns, and they answer
 * "is this current?" differently:
 *
 *   Expiring (TTL-absence)  the key's PRESENCE is the liveness signal. The
 *                           reader does not compute staleness at all — an
 *                           absent key means the source is offline.
 *                           (health 5 s, telemetry 15 s, dev_telemetry 5 s)
 *
 *   ts-owned                the key outlives its writer, so the READER owns
 *                           the policy: compare now - ts to the family's
 *                           window. (services 60 s, apttemps 300 s)
 *
 * On top of that, CD-6's three-state ladder — fresh -> idle -> stale — for
 * feeds where "no news" degrades in stages rather than flipping to offline.
 * Timestamps are the writer's clock, so per rules.md a NEGATIVE age (clock
 * skew) counts as fresh rather than as an error.
 */
#ifndef KDASH_FRESHNESS_H
#define KDASH_FRESHNESS_H

#include <stdbool.h>

/* Reader-owned staleness windows, as kpidash uses them today
 * (contracts/registry.md). */
#define KDASH_SERVICES_WINDOW_S 60
#define KDASH_APTTEMPS_WINDOW_S 300

/* The apartment-temperature bands kpidash has rendered since its apttemps
 * panel landed, and which kstudiodash 005 ported verbatim so that the two
 * panels would agree about what "warm" means.
 *
 * Agreeing by copy is exactly the drift CD-16 named for the claude ladder:
 * two dashboards deriving the same judgement is how they stop agreeing. So
 * the classification lives here and in kdash_apttemps_band(), and the panels
 * keep their palettes — CD-10, the library has no colours and never will.
 *
 * These are DEFAULTS, not policy baked into the derivation: the classifier
 * takes its thresholds by parameter, the way kdash_ladder() already does.
 *
 * The three numbers are BOUNDARIES, not band starts, and they reproduce
 * kpidash's `< 65 / <= 75 / < 80 / else` exactly: cold below COLD_F, ok from
 * COLD_F through OK_F inclusive, warm above OK_F and below HOT_F, hot at
 * HOT_F and above. */
#define KDASH_APTTEMPS_COLD_F 65.0
#define KDASH_APTTEMPS_OK_F   75.0
#define KDASH_APTTEMPS_HOT_F  80.0

/* kdash:panel:<host> is a COMMAND, and a command has a shorter useful life
 * than a status card: a panel that was down for an hour must come back to its
 * own screen rather than to whatever it was told while it was away. 60 s is
 * the same window `services` uses and is generous either way — a consumer
 * polling on a render tick sees a new command within seconds of it landing. */
#define KDASH_PANEL_WINDOW_S 60

/* Publisher cadences and TTLs for the expiring feeds, for callers that want to
 * pace their polling off the contract rather than off a guess. */
#define KDASH_HEALTH_TTL_S         5
#define KDASH_TELEMETRY_TTL_S      15
#define KDASH_DEV_TELEMETRY_TTL_S  5

/* The claude family's ladder thresholds (registry.md): a published session
 * status is trusted while fresh, idle at 15 min, stale at 40 min. The hooks
 * cannot report a killed process, which is why age — not the published word —
 * gets the last say.
 *
 * These are DEFAULTS, not policy baked into the derivation: every claude
 * helper in kdash_payload.h takes its thresholds by parameter, the way
 * kdash_ladder() already does. They live here rather than beside the parsers
 * because "when is a feed too old" is one question this header answers for
 * every family, and two dashboards disagreeing about it is the drift the
 * shared library exists to prevent. */
#define KDASH_CLAUDE_IDLE_S  (15 * 60)
#define KDASH_CLAUDE_STALE_S (40 * 60)

/* claude:limits is greyed per GAUGE, against that gauge's OWN stamp: stale
 * once the stamp is older than the writer's published cadence plus this grace.
 * A hash from a pre-cadence writer published no cadence at all, and falls back
 * to the fixed legacy window. */
#define KDASH_CLAUDE_LIMITS_GRACE_S 60
#define KDASH_CLAUDE_LIMITS_STALE_S (60 * 60)

typedef enum {
    KDASH_FRESH = 0, /* published state is current — trust it            */
    KDASH_IDLE,      /* no news for idle_s — probably parked             */
    KDASH_STALE,     /* no news for stale_s — probably gone              */
} kdash_freshness_t;

/* Seconds between a payload's `ts` and `now`, clamped at 0. Writer-clock skew
 * (a `ts` in the future) reads as age 0, i.e. as fresh — never as a huge or
 * negative age. */
long long kdash_age_s(double ts, long long now);

/* ts-owned staleness against one window. True when the value is older than
 * `window_s`. A non-positive window makes everything stale except skew. */
bool kdash_ts_stale(double ts, long long now, long long window_s);

/* The CD-6 ladder for an age in seconds. `idle_s` and `stale_s` are the
 * thresholds at which each state begins (inclusive). A stale_s below idle_s is
 * treated as idle_s, so a caller cannot configure an unreachable state. */
kdash_freshness_t kdash_ladder(long long age_s, long long idle_s,
                               long long stale_s);

/* Fixed lowercase label ("fresh", "idle", "stale"). Never NULL. */
const char *kdash_freshness_label(kdash_freshness_t f);

#endif /* KDASH_FRESHNESS_H */
