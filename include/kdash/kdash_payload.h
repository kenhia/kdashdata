/**
 * @file kdash_payload.h
 * Pure parsers for the schema'd payloads — the five kpidash feeds, the panel
 * control feed, and the three claude ones. No Redis, no sockets. Host-testable.
 *
 * **Two payload shapes, two parser signatures.** Every kpidash feed is a JSON
 * document under one key, so those parsers take a buffer and a length. The
 * claude family is the registry's first HASH-shaped one: `claude:session:*`
 * and `claude:limits` arrive as an HGETALL field/value list, so their parsers
 * take that list instead. The rules below are the same for both; only the way
 * the bytes arrive differs.
 *
 * The JSON Schema files in contracts/schemas/ are the source of truth; these
 * structs and parsers are their C projection, and the rules below are
 * contracts/rules.md's "Payloads" section made executable:
 *
 *   - A **required** field that is missing, of the wrong JSON type, or in
 *     violation of its schema constraint (e.g. `minimum: 0`) rejects the
 *     record WHOLE. The caller skips it and counts it; it never renders half
 *     a record.
 *   - An **optional** field that is malformed is treated as absent. The
 *     record survives — one bad optional must not cost the reading.
 *   - **Unknown fields are ignored.** That is the additive-evolution
 *     contract; a reader that rejected them would be the broken half.
 *   - Every parser zeroes `*out` before it starts, so a false return always
 *     leaves a fully-zeroed struct rather than a half-filled one.
 *
 * Buffers `json` need not be NUL-terminated; `len` bounds the parse and
 * embedded NULs are safe.
 */
#ifndef KDASH_PAYLOAD_H
#define KDASH_PAYLOAD_H

#include <stdbool.h>
#include <stddef.h>

#include "kdash/kdash_freshness.h"
#include "kdash/kdash_keys.h"

#define KDASH_OS_NAME_MAX  64
#define KDASH_GPU_NAME_MAX 48
#define KDASH_TEXT_MAX     128
#define KDASH_LABEL_MAX    32
#define KDASH_DISK_TYPE_MAX 8

/* claude-family display strings. Sized from kdeskdash's panel, which has
 * rendered them at these widths since its sprint 007. */
#define KDASH_PROJECT_MAX  48
#define KDASH_TITLE_MAX    96
#define KDASH_MODEL_MAX    32

/* Disks are a short list on a dashboard card; a longer one is truncated, not
 * an error (`disks_truncated` says so). */
#define KDASH_DISKS_MAX 8

/* ---- kpidash:client:<host>:health ---- */

typedef struct {
    char hostname[KDASH_TOKEN_MAX]; /* required; token contract enforced */
    double last_seen_ts;            /* required */
    bool has_uptime;
    double uptime_seconds;          /* optional */
    char os_name[KDASH_OS_NAME_MAX]; /* optional; "" when absent */
} kdash_health_t;

bool kdash_parse_health(const char *json, size_t len, kdash_health_t *out);

/* ---- kpidash:client:<host>:telemetry ---- */

typedef struct {
    char label[KDASH_LABEL_MAX];      /* required */
    char type[KDASH_DISK_TYPE_MAX];   /* optional; "" when absent/unknown */
    double used_gb, total_gb;         /* required */
    double pct;                       /* required, 0..100 */
} kdash_disk_t;

typedef struct {
    char hostname[KDASH_TOKEN_MAX]; /* required */
    double ts;                      /* required */
    double cpu_pct;                 /* required, >= 0 */
    double ram_used_mb;             /* required, >= 0 */
    double ram_total_mb;            /* required, >= 0 */
    bool has_top_core;
    double top_core_pct;            /* optional */

    /* "gpu" is an object, or null/absent when the host has none. */
    bool has_gpu;
    char gpu_name[KDASH_GPU_NAME_MAX];
    double gpu_vram_used_mb, gpu_vram_total_mb, gpu_compute_pct;

    int ndisks;                     /* entries filled in `disks`   */
    int disks_skipped;              /* items that failed their own required
                                     * fields — skipped, not fatal */
    bool disks_truncated;           /* more than KDASH_DISKS_MAX     */
    kdash_disk_t disks[KDASH_DISKS_MAX];
} kdash_telemetry_t;

bool kdash_parse_telemetry(const char *json, size_t len, kdash_telemetry_t *out);

/* ---- kpidash:client:<host>:dev_telemetry ---- */

typedef struct {
    char hostname[KDASH_TOKEN_MAX]; /* required */
    /* Optional routing field. Absent is legitimate (the schema says readers
     * treat it as "(legacy)"), so `host` is left "" rather than rejected. */
    char host[KDASH_TOKEN_MAX];
    double ts;                      /* required */
    double cpu_pct;                 /* required, >= 0 */
    double ram_used_mb;             /* required, >= 0 */
    double ram_total_mb;            /* required, >= 0 */
    bool has_top_core;
    double top_core_pct;            /* optional */
    bool has_gpu;                   /* true when gpu_compute_pct was present */
    double gpu_compute_pct;
    double gpu_vram_used_mb, gpu_vram_total_mb;
} kdash_dev_telemetry_t;

bool kdash_parse_dev_telemetry(const char *json, size_t len,
                               kdash_dev_telemetry_t *out);

/* ---- kpidash:services:<name>:<host> ---- */

typedef enum {
    KDASH_SVC_OK = 0,
    KDASH_SVC_UNHEALTHY,
    KDASH_SVC_MAINTENANCE,
    KDASH_SVC_DOWN,
    KDASH_SVC_UNKNOWN,
} kdash_service_state_t;

typedef struct {
    /* Identity comes from the KEY, which is authoritative over any payload
     * echo (schema note). The parser never writes these; the reader fills
     * them from kdash_service_key_parse(). `host` is "" for the `_` sentinel. */
    char name[KDASH_NAME_MAX];
    char host[KDASH_TOKEN_MAX];

    double ts;                    /* required */
    kdash_service_state_t state;  /* required; must be one of the enum words */
    char text[KDASH_TEXT_MAX];    /* required */
    bool has_icon;
    int icon;                     /* optional */
} kdash_service_t;

/* Parses only the payload half; `out->name`/`out->host` are left untouched so
 * the reader can fill them from the key either before or after this call. */
bool kdash_parse_service(const char *json, size_t len, kdash_service_t *out);

/* Map a `state` string to the enum. False when it is not one of the five
 * words — the schema's enum is closed, so an unrecognised state is a rejected
 * record, not a silent KDASH_SVC_UNKNOWN. */
bool kdash_service_state_from_str(const char *s, kdash_service_state_t *out);
const char *kdash_service_state_str(kdash_service_state_t s);

/* ---- kpidash:apttemps:<zone> ---- */

typedef struct {
    char zone[KDASH_ZONE_MAX];       /* from the key; authoritative     */
    char label[KDASH_ZONE_MAX];      /* payload display label; "" absent */
    double temp_f;                   /* required */
    double humidity_pct;             /* required */
    double ts;                       /* required */
} kdash_apttemps_t;

/* Parses only the payload half; `out->zone` is left untouched (see above). */
bool kdash_parse_apttemps(const char *json, size_t len, kdash_apttemps_t *out);

/* ---- kdash:panel:<host> ---- */

/* Which screen the panel should be showing. The schema's enum is closed: an
 * unrecognised word rejects the record rather than being guessed at, exactly
 * as the claude `status` enum does. */
typedef enum {
    KDASH_PANEL_DASH = 0,
    KDASH_PANEL_DESKTOP,
} kdash_panel_want_t;

typedef struct {
    /* Identity comes from the KEY and the payload carries no echo of it, so
     * the reader fills this from the host it built the key with. */
    char host[KDASH_TOKEN_MAX];

    kdash_panel_want_t want; /* required; one of the closed enum's words */
    double ts;               /* required AND positive — see below        */
} kdash_panel_t;

/* Parses only the payload half; `out->host` is left untouched so the reader
 * can fill it from the key either before or after this call.
 *
 * `ts` must be present, numeric and POSITIVE — stricter than the other
 * ts-owned families, and deliberately so. Here the stamp is not merely the
 * record's age: it is the command's IDENTITY, because a consumer acts on `ts`
 * advancing. A record whose stamp cannot be compared is not a weakly-dated
 * command, it is no command at all, and 0 is already this repo's spelling of
 * "unknown stamp" (see the claude field helpers). */
bool kdash_parse_panel(const char *json, size_t len, kdash_panel_t *out);

/* Map a `want` string to the enum, and back to the schema's own word. */
bool kdash_panel_want_from_str(const char *s, kdash_panel_want_t *out);
const char *kdash_panel_want_str(kdash_panel_want_t w);

/* Should this command be acted on now?
 *
 * True when `cmd` carries a stamp STRICTLY newer than `acted_ts` — the stamp
 * of the last command the caller acted on, or 0 if it has acted on none — and
 * that stamp is no older than `window_s`. KDASH_PANEL_WINDOW_S is the family's
 * default; it is a parameter for the same reason every other threshold here is
 * one (CD-16).
 *
 * Both halves earn their place, and this is the whole reason the family is
 * ts-owned state rather than a GETDEL one-shot (CD-17):
 *
 *   - Strictly newer, not merely different, makes a republish idempotent and
 *     makes the feed EDGE-triggered rather than level-triggered. That is what
 *     stops a dashboard fighting a human at the keyboard: once someone
 *     switches the screen by hand, a `want` still sitting in Redis must not
 *     yank it back.
 *   - The window stops a restart replaying an old switch: a panel that was off
 *     for an hour comes back to its own screen.
 *
 * A zeroed record — the one a rejected parse or a KDASH_ABSENT read leaves
 * behind — is never actionable, so a caller that ignores the status and just
 * asks this question still behaves correctly. */
bool kdash_panel_actionable(const kdash_panel_t *cmd, double acted_ts,
                            long long now, long long window_s);

/* ---- claude:session:<host>:<sid> (HASH) ---- */

/* Published status, written by the Claude Code hooks. `blocked` means the
 * agent is sitting on an AskUserQuestion and cannot proceed without the user.
 * The schema's enum is closed: an unrecognised word distrusts the whole
 * record rather than defaulting to anything. */
typedef enum {
    KDASH_CLAUDE_WORKING = 0,
    KDASH_CLAUDE_AWAITING,
    KDASH_CLAUDE_BLOCKED,
} kdash_claude_status_t;

/* Derived display state — published status tempered by age. The enum order IS
 * the attention-first sort rank, which is what kdash_claude_sessions_refresh()
 * sorts on. */
typedef enum {
    KDASH_CLAUDE_DISP_BLOCKED = 0, /* fresh `blocked` — hard-blocked on you  */
    KDASH_CLAUDE_DISP_AWAITING,    /* fresh `awaiting` — your turn           */
    KDASH_CLAUDE_DISP_WORKING,     /* fresh `working`                        */
    KDASH_CLAUDE_DISP_IDLE,        /* no event for idle_s — probably parked  */
    KDASH_CLAUDE_DISP_STALE,       /* no event for stale_s — probably gone   */
} kdash_claude_disp_t;

typedef struct {
    /* Identity comes from the KEY (schema note), so the reader fills these
     * from kdash_claude_session_key_parse() and the parser copies them in. */
    char host[KDASH_TOKEN_MAX];
    char sid[KDASH_TOKEN_MAX];

    /* Optional display fields; "" when absent. The library supplies no
     * placeholder for an absent project — a panel's "?" is rendering, and
     * CD-10 keeps that on the panel's side. */
    char project[KDASH_PROJECT_MAX];
    char title[KDASH_TITLE_MAX];
    char model[KDASH_MODEL_MAX];

    double ts;         /* required, positive: last lifecycle event or keepalive */
    double started_ts; /* optional; 0 when unknown                              */

    kdash_claude_status_t status; /* required                                  */
    kdash_claude_disp_t disp;     /* derived — see sessions_refresh()          */
} kdash_claude_session_t;

/* Build a session record from an HGETALL field/value list. `host` and `sid`
 * come from the already-validated key and are revalidated here.
 *
 * `status` and a positive numeric `ts` are the whole liveness contract, and a
 * record missing either is rejected outright. That is the resurrection-race
 * guard, not fussiness: a statusline write landing after SessionEnd leaves a
 * hash with no status, and a reader that filled in a default would raise a
 * finished session from the dead. A keepalive refreshes `ts` alone, so a hash
 * carrying only `ts` is a legitimate transient that correctly renders as
 * nothing at all.
 *
 * Unknown fields are ignored (additive evolution). `cwd` is deliberately among
 * them: `project` is already its basename, and no dashboard renders the path.
 *
 * Every value arrives off the wire as a string; `fields[i]`/`values[i]` must be
 * NUL-terminated and neither may be NULL for the pair to be read. */
bool kdash_parse_claude_session(const char *host, const char *sid,
                                const char *const *fields,
                                const char *const *values, int nfields,
                                kdash_claude_session_t *out);

/* Map a `status` string to the enum, and back to the schema's own word. */
bool kdash_claude_status_from_str(const char *s, kdash_claude_status_t *out);
const char *kdash_claude_status_str(kdash_claude_status_t s);

/* Fixed lowercase label for a display state ("blocked", "awaiting",
 * "working", "idle", "stale"). This is the enum's NAME, in the schema's own
 * vocabulary — the same thing kdash_service_state_str() hands back, and not a
 * display string: a panel that wants "BLOCKED ON YOU" writes that itself. */
const char *kdash_claude_disp_str(kdash_claude_disp_t d);

/* Derive the display state for a published status at `age_s` seconds old.
 *
 * Composed on kdash_ladder(), which owns the time bands: stale is stale
 * whatever the record claims, and only `working` degrades to idle — `blocked`
 * and `awaiting` both mean "your turn", and a turn does not stop being yours
 * because you took twenty minutes over it, so they stay prominent until the
 * ladder says stale.
 *
 * Thresholds are parameters, with KDASH_CLAUDE_IDLE_S / KDASH_CLAUDE_STALE_S
 * as the family's defaults. */
kdash_claude_disp_t kdash_claude_display(kdash_claude_status_t status,
                                         long long age_s, long long idle_s,
                                         long long stale_s);

/* Derive every record's `disp` for `now` and sort attention-first: rank
 * ascending, then most-recent `ts` first, then host/sid lexicographic so the
 * order is stable across renders. Ordering is part of the data model — "which
 * of these needs me most" has one right answer, and deriving it twice is how
 * two dashboards drift. */
void kdash_claude_sessions_refresh(kdash_claude_session_t *arr, int n,
                                   long long now, long long idle_s,
                                   long long stale_s);

/* ---- claude:limits (HASH) ---- */

typedef struct {
    bool valid; /* false unless both required percentages parsed */

    double five_hour_pct;       /* required, clamped to [0,100]           */
    double seven_day_pct;       /* required, clamped to [0,100]           */
    double five_hour_resets_at; /* optional; 0 = unknown                  */
    double seven_day_resets_at; /* optional; 0 = unknown                  */

    /* The OBSERVATION time behind the headline fields, not the publish time:
     * several writers on several hosts share this one key, and a writer must
     * not publish over a fresher one. */
    double updated_at;
    double expected_refresh_s; /* writer's cadence; 0 = none published */

    /* The model-scoped weekly window, with its OWN stamp and cadence. Only an
     * oauth poll can supply these, so a shared stamp would let a statusline
     * write make a frozen scoped gauge look fresh. */
    bool scoped_valid; /* label and percentage both present */
    char scoped_model[KDASH_MODEL_MAX]; /* display string — render, never match */
    double scoped_pct;
    double scoped_resets_at;
    bool scoped_active; /* this window is the binding constraint */
    double scoped_updated_at;        /* 0 = stampless, i.e. always stale */
    double scoped_expected_refresh_s;
} kdash_claude_limits_t;

/* Parse the claude:limits hash from an HGETALL field/value list. Both
 * `*_pct` fields are required, numeric and clamped to [0,100]; anything else
 * rejects the record whole and leaves `*out` zeroed. Half a scoped set (a
 * percentage with no label, or the reverse) yields `scoped_valid == false`
 * rather than an unlabelled gauge. */
bool kdash_parse_claude_limits(const char *const *fields,
                               const char *const *values, int nfields,
                               kdash_claude_limits_t *out);

/* True when the headline snapshot is valid but its own stamp is older than the
 * writer's cadence plus `grace_s` — or than `legacy_window_s` when the writer
 * published no cadence at all. Defaults: KDASH_CLAUDE_LIMITS_GRACE_S and
 * KDASH_CLAUDE_LIMITS_STALE_S. */
bool kdash_claude_limits_stale(const kdash_claude_limits_t *l, long long now,
                               long long grace_s, long long legacy_window_s);

/* The same rule for the scoped set against ITS stamp and ITS cadence. A scoped
 * set with no stamp at all is ALWAYS stale: it must never borrow the
 * headline's freshness. False when there is no scoped set to judge. */
bool kdash_claude_limits_scoped_stale(const kdash_claude_limits_t *l,
                                      long long now, long long grace_s,
                                      long long legacy_window_s);

/* ---- claude:recent (list of JSON documents) ---- */

typedef struct {
    char host[KDASH_TOKEN_MAX];      /* required; token contract enforced  */
    char project[KDASH_PROJECT_MAX]; /* required, non-empty                */
    char title[KDASH_TITLE_MAX];     /* optional; "" when never generated  */
    double ended_ts;                 /* unix s the session ended           */
    double dur_s;                    /* 0 = unknown                        */
} kdash_claude_recent_t;

/* Parse one claude:recent element. Unlike its two siblings this really is a
 * JSON document, so it takes a buffer and a length like every kpidash parser.
 * `host` and a non-empty `project` are required; a record failing either is
 * rejected whole. */
bool kdash_parse_claude_recent(const char *json, size_t len,
                               kdash_claude_recent_t *out);

#endif /* KDASH_PAYLOAD_H */
