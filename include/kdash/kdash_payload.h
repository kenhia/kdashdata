/**
 * @file kdash_payload.h
 * Pure parsers for the schema'd payloads — the five kpidash feeds, the three
 * panel control feeds, the three claude ones and the two status feeds (CD-28).
 * No Redis, no sockets.
 * Host-testable.
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

/* Which band a zone's temperature falls in (CD-16 applied to apttemps; see
 * kdash_freshness.h for the thresholds and the argument).
 *
 * `stale` is the caller's own ts-owned verdict — kdash_ts_stale() against
 * KDASH_APTTEMPS_WINDOW_S — and it WINS over the number, because a reading
 * nobody has refreshed in five minutes is not evidence about the room. Both
 * panels already grey it; this makes that ordering the library's, not each
 * panel's.
 *
 * Vocabulary, not colour: which colour a band renders as stays in the panel
 * (CD-10). Thresholds are parameters, with KDASH_APTTEMPS_{COLD,OK,HOT}_F as
 * the defaults. Boundaries arriving out of order are clamped rather than
 * rejected, so a caller cannot configure a band that can never be reached —
 * the same guarantee kdash_ladder() gives for stale_s below idle_s. */
typedef enum {
    KDASH_TEMP_COLD = 0, /* below COLD_F                          */
    KDASH_TEMP_OK,       /* COLD_F .. OK_F inclusive              */
    KDASH_TEMP_WARM,     /* above OK_F, below HOT_F               */
    KDASH_TEMP_HOT,      /* HOT_F and above                       */
    KDASH_TEMP_STALE,    /* too old to judge — `stale` was true  */
} kdash_temp_band_t;

kdash_temp_band_t kdash_apttemps_band(double temp_f, bool stale, double cold_f,
                                      double ok_f, double hot_f);

/* Fixed lowercase label ("cold", "ok", "warm", "hot", "stale"). Never NULL. */
const char *kdash_temp_band_label(kdash_temp_band_t b);

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

/* The same question, family-agnostic: the two bullets above are the rule for
 * every ts-owned COMMAND feed, and kdash_panel_actionable() is this function
 * with a kdash_panel_t unwrapped for it. New control families call this
 * directly rather than each growing a one-line wrapper — the point of CD-17
 * was one right answer to "have I acted on this already", not one per family.
 *
 * `cmd_ts` of 0 — what a rejected parse or a KDASH_ABSENT read leaves — is
 * never actionable. */
bool kdash_cmd_actionable(double cmd_ts, double acted_ts, long long now,
                          long long window_s);

/* ---- kdash:panelmode:<host> ---- */

/* Mode ids and setting names are bounded by the schema's own patterns, so
 * these buffers are exactly big enough for a conforming record and a
 * non-conforming one is rejected rather than truncated into place. */
#define KDASH_MODE_MAX          32 /* 31 chars + NUL */
#define KDASH_SETTING_NAME_MAX  32 /* 31 chars + NUL */
#define KDASH_SETTING_VALUE_MAX 64 /* 63 chars + NUL */
#define KDASH_SETTINGS_MAX      24 /* the schema's maxProperties */

/* One entry of the command's optional `settings` object. Name and meaning
 * belong to the MODE and never to this contract (CD-22); the value is TEXT,
 * exactly as the Redis HASH this replaced delivered it, so there is no
 * number-formatting rule for two consumers to disagree about. */
typedef struct {
    char name[KDASH_SETTING_NAME_MAX];
    char value[KDASH_SETTING_VALUE_MAX];
} kdash_setting_t;

typedef struct {
    /* Identity comes from the KEY, exactly as kdash_panel_t's does. */
    char host[KDASH_TOKEN_MAX];

    /* Required. The dashboard's own screen vocabulary — this library does not
     * know which modes exist, and a mode a dashboard does not have is a
     * command it ignores. */
    char mode[KDASH_MODE_MAX];

    /* Optional one-shot configuration for `mode`. Absent is the common case
     * and is NOT an error: an empty list means "switch, change nothing". */
    kdash_setting_t settings[KDASH_SETTINGS_MAX];
    int  settings_count;
    bool settings_truncated; /* more than KDASH_SETTINGS_MAX arrived      */
    int  settings_skipped;   /* entries whose name or value broke the shape */

    double ts; /* required AND positive, exactly as kdash_panel_t's is */
} kdash_panelmode_t;

/* Parses only the payload half; `out->host` is left untouched, as
 * kdash_parse_panel() leaves it.
 *
 * `settings` is an OPTIONAL field, so the rules apply in their usual
 * asymmetry: a `settings` that is not an object is treated as absent and the
 * command still stands, and an individual entry whose name or value breaks the
 * schema's shape is dropped and counted in `settings_skipped` rather than
 * costing the whole command. A mode switch arriving with one unreadable knob
 * is still a mode switch. */
bool kdash_parse_panelmode(const char *json, size_t len,
                           kdash_panelmode_t *out);

/* Look one setting up by name; NULL when the command did not carry it.
 *
 * That NULL is the contract, not a miss to work around: every name ABSENT
 * keeps whatever the mode would have used, which is what makes a partial
 * injection safe and is exactly what HGETALL-and-apply-present-fields did
 * before this feed existed. */
const char *kdash_setting_get(const kdash_panelmode_t *cmd, const char *name);

/* ---- kdash:panelshot:<host> ---- */

#define KDASH_PATH_MAX 256 /* 255 chars + NUL, the schema's maxLength */

typedef struct {
    /* Identity comes from the KEY. */
    char host[KDASH_TOKEN_MAX];

    /* Optional; "" when absent, which means the dashboard's own default and is
     * the ordinary case. When present it is ABSOLUTE — the schema enforces the
     * leading '/', so it can never resolve against whatever the dashboard's
     * working directory happens to be. The rest of the policy is the panel's:
     * this key is writable by every holder of the central Redis password, so a
     * dashboard should refuse a path outside a directory it owns (CD-23). */
    char path[KDASH_PATH_MAX];

    double ts; /* required AND positive */
} kdash_panelshot_t;

/* Parses only the payload half; `out->host` is left untouched. A `path` that
 * is not an absolute string is treated as absent — the optional-field rule —
 * so a malformed path costs the caller its default, never the capture. */
bool kdash_parse_panelshot(const char *json, size_t len,
                           kdash_panelshot_t *out);

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

    /* The source reported NO five-hour window (CD-29): the wire's
     * `five_hour_none` is 1. When set, five_hour_pct and five_hour_resets_at
     * are forced to 0 whatever the hash holds, so a leftover from an earlier
     * writer can never render as a reading. False on a record that predates
     * the field (its five_hour_pct is a real reading) and on a rejected or
     * absent record — "no window" is only ever a positive statement, so it
     * cannot be confused with "unknown", which is `valid == false`. Rides the
     * headline stamp: kdash_claude_limits_stale() judges it too. */
    bool five_hour_none;

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
 * rather than an unlabelled gauge. `five_hour_none` is set only by a value
 * that parses as exactly 1; 0, absent or unparseable leaves the five-hour
 * fields as published. */
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

/* ---- the status feeds (CD-28) -------------------------------------------
 *
 * kdash:korg:pulse and kdash:klams:status are one JSON document under one
 * fixed key each, ts-owned, and cadence-declared: the writer publishes
 * `expected_refresh_s` and the caller judges age with kdash_cadence_stale().
 *
 * Their arrays are CAPPED by the schema, and an over-long one is not a
 * rejected record. The parser keeps what the cap allows and sets
 * `*_truncated`, the same way telemetry's `disks` and panelmode's `settings`
 * already behave, because a card that can draw 24 projects should still draw
 * them when a publisher sent 25. Which end it keeps is per array, and chosen
 * so the kept part is the part a card draws: the FIRST entries of a
 * largest-first list, the NEWEST entries of an oldest-first series (a series
 * truncated from the end would lose today). A malformed entry is skipped and
 * counted in `*_skipped`; the record survives it.
 *
 * Nullable values are present-and-null on the wire (CD-27). Here a null count
 * is KDASH_COUNT_NULL and a null stamp is 0, the library's existing spelling
 * for an unknown stamp. An ABSENT required field still rejects the record:
 * absent and null are different statements. */

/* "The producer could not measure this." Counts are never negative on the
 * wire (every schema says `minimum: 0`), so -1 cannot collide with a value. */
#define KDASH_COUNT_NULL (-1)

#define KDASH_DATE_MAX 11 /* `YYYY-MM-DD` + NUL */

/* ---- kdash:korg:pulse ---- */

#define KDASH_KORG_PROJECTS_MAX 24 /* the schema's maxItems */
#define KDASH_KORG_HISTORY_MAX  30 /* the schema's maxItems */

typedef struct {
    char name[KDASH_PROJECT_MAX]; /* korg project name; schema maxLength 47 */
    int open;                     /* >= 1 */
} kdash_korg_project_t;

typedef struct {
    char date[KDASH_DATE_MAX]; /* `YYYY-MM-DD`, korg's board timezone */
    int open;                  /* backlog at that day's end */
} kdash_korg_day_t;

typedef struct {
    double ts;                 /* required, positive */
    double expected_refresh_s; /* required, >= 1 */

    int open;       /* korg's backlog: non-terminal, unarchived, all projects */
    int projects;   /* projects holding at least one open item               */
    int awaiting;   /* the Awaiting Ken lane                                 */
    int active;     /* proposals in status `active`                          */
    int queued;     /* proposals in status `proposed`                        */
    int other_open; /* open items no `by_project` entry shows                */

    /* Largest first, as the writer sent it. by_project[0] is the top project
     * when by_project_count > 0. */
    kdash_korg_project_t by_project[KDASH_KORG_PROJECTS_MAX];
    int  by_project_count;
    int  by_project_skipped;
    bool by_project_truncated; /* the first 24 were kept */

    /* Oldest first; history[history_count - 1] is today when non-empty. */
    kdash_korg_day_t history[KDASH_KORG_HISTORY_MAX];
    int  history_count;
    int  history_skipped;
    bool history_truncated; /* the NEWEST 30 were kept */
} kdash_korg_pulse_t;

/* Every top-level field is required, the two arrays included (an empty array
 * is a statement; an absent one is not). Counts must be whole numbers >= 0. */
bool kdash_parse_korg_pulse(const char *json, size_t len,
                            kdash_korg_pulse_t *out);

/* ---- kdash:klams:status ---- */

#define KDASH_KLAMS_NAME_MAX       32 /* 31 chars + NUL, the schema's maxLength */
#define KDASH_KLAMS_SCANNERS_MAX   8
#define KDASH_KLAMS_SUBSYSTEMS_MAX 8
#define KDASH_KLAMS_DAYS_MAX       14

/* klams's own verdict, copied. Closed: an unknown word rejects the record. */
typedef enum {
    KDASH_KLAMS_OK = 0,   /* every subsystem answers                      */
    KDASH_KLAMS_DEGRADED, /* klams answers, a subsystem does not          */
    KDASH_KLAMS_DOWN,     /* klams did not answer its publisher           */
} kdash_klams_health_t;

typedef struct {
    char name[KDASH_KLAMS_NAME_MAX];
    double last_wrote_at;       /* 0 = never wrote                          */
    double last_ran_at;         /* 0 = the publisher has no source for it   */
    double expected_interval_s; /* >= 1: the scanner's schedule             */
} kdash_klams_scanner_t;

typedef struct {
    char name[KDASH_KLAMS_NAME_MAX];
    bool ok;
} kdash_klams_subsystem_t;

typedef struct {
    char date[KDASH_DATE_MAX];
    long long added; /* knowledge records created that day */
} kdash_klams_day_t;

typedef struct {
    double ts;                 /* required, positive */
    double expected_refresh_s; /* required, >= 1 */
    kdash_klams_health_t health;

    char version[KDASH_KLAMS_NAME_MAX]; /* "" when null */

    /* KDASH_COUNT_NULL when klams could not supply them. */
    long long uptime_s;
    long long knowledge;
    long long facts;
    long long events;
    long long queue_depth;
    long long queue_capacity;

    double backup_at; /* 0 = no backup known */

    kdash_klams_scanner_t scanners[KDASH_KLAMS_SCANNERS_MAX];
    int  scanners_count;
    int  scanners_skipped;
    bool scanners_truncated; /* the first 8 were kept */

    kdash_klams_subsystem_t subsystems[KDASH_KLAMS_SUBSYSTEMS_MAX];
    int  subsystems_count;
    int  subsystems_skipped;
    bool subsystems_truncated; /* the first 8 were kept */

    /* Oldest first; the last is today when non-empty. */
    kdash_klams_day_t new_per_day[KDASH_KLAMS_DAYS_MAX];
    int  new_per_day_count;
    int  new_per_day_skipped;
    bool new_per_day_truncated; /* the NEWEST 14 were kept */
} kdash_klams_status_t;

/* Every top-level field is required, nullable ones included: a `down` record
 * carries them all as null. A scanner entry missing `last_ran_at` (rather than
 * carrying null) is skipped, which is the schema's rule at entry level. */
bool kdash_parse_klams_status(const char *json, size_t len,
                              kdash_klams_status_t *out);

/* Map a `health` string to the enum, and back to the schema's own word. */
bool kdash_klams_health_from_str(const char *s, kdash_klams_health_t *out);
const char *kdash_klams_health_str(kdash_klams_health_t h);

/* ---- kdash:calendar (CD-30) ---------------------------------------------
 *
 * The tagged days for the time and calendar card: Ken's days off, holidays
 * and special days, already expanded by the publisher to concrete dates. Every
 * date is `YYYY-MM-DD` in America/Los_Angeles, so a caller compares strings and
 * does no calendar arithmetic -- but it must take `today` in that zone, not
 * from a UTC system clock.
 *
 * Freshness is NOT ts-first here. Tags change rarely, so a caller keeps
 * drawing the last good record; kdash_cadence_stale() on `ts` is a muted hint
 * at most. The real stale condition is a window that no longer covers today:
 * kdash_calendar_covers() false, and then the card draws its grid untagged. */

#define KDASH_CALENDAR_DAYS_MAX  64 /* the schema's maxItems             */
/* The schema bounds names in CHARACTERS; the buffers hold that many UTF-8
 * characters of up to 4 bytes each, plus the NUL (sprint 026). */
#define KDASH_CALENDAR_NAME_CHARS  31 /* `holiday`/`special` */
#define KDASH_CALENDAR_SHORT_CHARS 12 /* `short`             */
#define KDASH_CALENDAR_NAME_MAX  (KDASH_CALENDAR_NAME_CHARS * 4 + 1)
#define KDASH_CALENDAR_SHORT_MAX (KDASH_CALENDAR_SHORT_CHARS * 4 + 1)

typedef struct {
    char date[KDASH_DATE_MAX];               /* `YYYY-MM-DD`               */
    bool off;                                /* a personal day off         */
    char holiday[KDASH_CALENDAR_NAME_MAX];   /* "" when not a holiday      */
    char special[KDASH_CALENDAR_NAME_MAX];   /* "" when not a special day  */
    char short_label[KDASH_CALENDAR_SHORT_MAX]; /* wire `short`; "" = none */
} kdash_calendar_day_t;

typedef struct {
    double ts;                 /* required, positive */
    double expected_refresh_s; /* required, >= 1     */

    /* The window, inclusive. Inside it a date with no entry is untagged;
     * outside it a date is unknown. from <= to always holds after a parse. */
    char from[KDASH_DATE_MAX];
    char to[KDASH_DATE_MAX];

    /* Ascending by date, as the writer sent it. */
    kdash_calendar_day_t days[KDASH_CALENDAR_DAYS_MAX];
    int  days_count;
    int  days_skipped;   /* malformed, untagged, or outside the window */
    bool days_truncated; /* the FIRST (earliest) 64 were kept          */
} kdash_calendar_t;

/* Every top-level field is required, `days` included (empty is a statement).
 * A missing, malformed or inverted window rejects the record. An entry is
 * kept only if it has a valid `date` and boolean `off`, carries at least one
 * tag (`off` true, `holiday` or `special`), and lies inside the window; any
 * other entry is skipped and counted. A string tag that is present must be a
 * non-empty string within its bound, or the entry is malformed. */
bool kdash_parse_calendar(const char *json, size_t len, kdash_calendar_t *out);

/* True when `date` (`YYYY-MM-DD`) lies inside the record's window. False for
 * a NULL argument or a zeroed (unparsed) record. Pass today's local date: false
 * is the calendar's stale condition. */
bool kdash_calendar_covers(const kdash_calendar_t *cal, const char *date);

/* The entry for `date`, or NULL when it is untagged (or outside the window, or
 * either argument is NULL). Linear: at most 64 entries. */
const kdash_calendar_day_t *kdash_calendar_day(const kdash_calendar_t *cal,
                                               const char *date);

/* ---- kdash:agents (CD-31) -----------------------------------------------
 *
 * The last `window_s` (24 h) of agent sessions as bars, for kpidash's Agents
 * card. A bar is a STRETCH of activity, not a whole session: one session may
 * produce several, so `id` names a bar. A leg hangs from its overseer's bar
 * through `parent`.
 *
 * Freshness is ts-first, judged with kdash_cadence_stale() on the record's own
 * `expected_refresh_s`. A still-running bar (`working`/`waiting`) carries the
 * publish time as its `end`; the card draws the now line from its own clock and
 * runs such a bar to now while the record is fresh -- kdash_agents_bar_end()
 * is that rule, so the card does not re-derive it. */

#define KDASH_AGENTS_BARS_MAX 96  /* the schema's maxItems                    */
#define KDASH_AGENTS_ID_MAX   24  /* 23 token chars + NUL, `id` and `parent`  */
#define KDASH_AGENTS_HOST_MAX 16  /* 15 token chars + NUL                     */
#define KDASH_AGENTS_NAME_MAX 128 /* 31 UTF-8 characters (<= 4 bytes) + NUL  */
#define KDASH_AGENTS_NAME_CHARS 31

typedef enum {
    KDASH_AGENT_DONE = 0, /* the stretch ended at `end`                    */
    KDASH_AGENT_WORKING,  /* still running                                  */
    KDASH_AGENT_WAITING,  /* still open, and owes Ken an okay or a decision */
} kdash_agent_state_t;

typedef struct {
    char id[KDASH_AGENTS_ID_MAX];
    char host[KDASH_AGENTS_HOST_MAX];
    char name[KDASH_AGENTS_NAME_MAX];
    double start;              /* unix seconds; may predate the window */
    double end;                /* unix seconds; >= start; for a running bar,
                                * the publish time -- see kdash_agents_bar_end() */
    kdash_agent_state_t state;
    bool shipped;              /* a ship happened at `end`  */
    bool overseer;             /* this bar is an overseer's */
    /* The overseer bar this one hangs from: "" and -1 when there is none, or
     * when the wire's `parent` did not name a kept overseer bar (dropped). */
    char parent[KDASH_AGENTS_ID_MAX];
    int  parent_index;
} kdash_agent_bar_t;

typedef struct {
    double ts;                 /* required, positive */
    double expected_refresh_s; /* required, 1..86400 */
    double window_s;           /* required, 3600..604800; 86400 for the card */

    /* Ascending by `start`, as the writer sent it. */
    kdash_agent_bar_t bars[KDASH_AGENTS_BARS_MAX];
    int  bars_count;
    int  bars_skipped;    /* malformed, duplicate id, or ended before the window */
    bool bars_truncated;  /* the LAST (newest) 96 were kept               */
    int  parents_dropped; /* `parent` present but naming no kept overseer bar */
} kdash_agents_t;

/* Every top-level field is required, `bars` included (empty is a statement:
 * no agent ran in the window). A bar is kept only if every required field is
 * well-formed, its `state` is one of the three words, `start` <= `end`, its
 * `end` is not before `ts - window_s`, and its `id` is not already taken; any
 * other bar is skipped and counted. An optional `parent` that does not name a
 * kept bar with `overseer` true (after truncation) is dropped and counted, and
 * the bar itself is kept. */
bool kdash_parse_agents(const char *json, size_t len, kdash_agents_t *out);

/* Map a `state` string to the enum, and back to the schema's own word. */
bool kdash_agent_state_from_str(const char *s, kdash_agent_state_t *out);
const char *kdash_agent_state_str(kdash_agent_state_t s);

/* Where the card draws bar `b`'s right edge, given its own clock `now`.
 * `done`: `end`, clipped to now. `working`/`waiting`: now, while the record is
 * fresh (not kdash_cadence_stale() with KDASH_CADENCE_GRACE_S); once the
 * record is stale, `end` clipped to now -- a dead publisher must not keep
 * stretching bars it last saw an hour ago. 0 for a NULL argument. */
double kdash_agents_bar_end(const kdash_agents_t *a, const kdash_agent_bar_t *b,
                            long long now);

#endif /* KDASH_PAYLOAD_H */
