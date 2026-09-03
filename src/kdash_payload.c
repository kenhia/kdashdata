/**
 * @file kdash_payload.c
 * Pure payload parsers — no Redis, no sockets. Host-testable.
 *
 * Every parser follows the same shape, which is the rules.md contract:
 * zero the output, parse, take the required fields (rejecting the record
 * whole if any is missing/mistyped/out of range), then take the optional
 * fields best-effort. Unknown fields are never looked at, which is exactly
 * how a reader stays additive-evolution-safe.
 *
 * Two shapes of input, one set of rules. The kpidash feeds and claude:recent
 * are JSON documents and go through cJSON; claude:session and claude:limits
 * are Redis HASHes and arrive as an HGETALL field/value list of strings, which
 * is why the second half of this file has its own small field helpers.
 */
#include "kdash/kdash_payload.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

/* ---- small field helpers ------------------------------------------------ */

/* Required number. False if absent, non-numeric, or below `min` (pass a very
 * negative min for "no lower bound"). */
static bool req_num(const cJSON *root, const char *field, double min,
                    double *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsNumber(o))
        return false;
    if (o->valuedouble < min)
        return false;
    *out = o->valuedouble;
    return true;
}

/* Optional number. Sets *out and returns true only when present, numeric and
 * >= min; a malformed optional is simply absent. */
static bool opt_num(const cJSON *root, const char *field, double min,
                    double *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsNumber(o) || o->valuedouble < min)
        return false;
    *out = o->valuedouble;
    return true;
}

/* Copy a JSON string field into a fixed buffer, truncating to fit. Truncation
 * is right for DISPLAY strings (a status line, an OS name) and wrong for
 * identity, which is why the token fields below use req_token() instead. */
static bool copy_str(const cJSON *root, const char *field, char *out,
                     size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring)
        return false;
    size_t n = strlen(o->valuestring);
    if (n > outsz - 1)
        n = outsz - 1;
    memcpy(out, o->valuestring, n);
    out[n] = '\0';
    return true;
}

/* Required host-token field: must be a string AND satisfy the key grammar's
 * token contract, because it is identity and it reaches key construction. */
static bool req_token(const cJSON *root, const char *field, char *out,
                      size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring)
        return false;
    size_t n = strlen(o->valuestring);
    if (!kdash_token_ok(o->valuestring, n) || n + 1 > outsz)
        return false;
    memcpy(out, o->valuestring, n);
    out[n] = '\0';
    return true;
}

/* Parse into a cJSON root, rejecting anything that is not a JSON object. */
static cJSON *parse_object(const char *json, size_t len) {
    if (!json || len == 0)
        return NULL;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root)
        return NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* ---- health ------------------------------------------------------------- */

bool kdash_parse_health(const char *json, size_t len, kdash_health_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_token(root, "hostname", out->hostname, sizeof(out->hostname)) &&
              req_num(root, "last_seen_ts", -1e18, &out->last_seen_ts);

    if (ok) {
        out->has_uptime = opt_num(root, "uptime_seconds", 0.0, &out->uptime_seconds);
        (void)copy_str(root, "os_name", out->os_name, sizeof(out->os_name));
    } else {
        memset(out, 0, sizeof(*out));
    }

    cJSON_Delete(root);
    return ok;
}

/* ---- telemetry ---------------------------------------------------------- */

/* One entry of the optional `disks` array. A malformed entry is skipped and
 * counted by the caller; it never rejects the whole sample. */
static bool parse_disk(const cJSON *item, kdash_disk_t *out) {
    memset(out, 0, sizeof(*out));
    if (!cJSON_IsObject(item))
        return false;
    if (!copy_str(item, "label", out->label, sizeof(out->label)))
        return false;
    if (!req_num(item, "used_gb", 0.0, &out->used_gb))
        return false;
    if (!req_num(item, "total_gb", 0.0, &out->total_gb))
        return false;
    if (!req_num(item, "pct", 0.0, &out->pct) || out->pct > 100.0)
        return false;
    (void)copy_str(item, "type", out->type, sizeof(out->type));
    return true;
}

static void parse_disks(const cJSON *root, kdash_telemetry_t *out) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "disks");
    if (!cJSON_IsArray(arr))
        return;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (out->ndisks >= KDASH_DISKS_MAX) {
            out->disks_truncated = true;
            break;
        }
        if (parse_disk(item, &out->disks[out->ndisks]))
            out->ndisks++;
        else
            out->disks_skipped++;
    }
}

/* The telemetry `gpu` member is an object, or null/absent on a host with no
 * GPU. Only an object sets has_gpu; null and absent are the same answer. */
static void parse_gpu(const cJSON *root, kdash_telemetry_t *out) {
    const cJSON *gpu = cJSON_GetObjectItemCaseSensitive(root, "gpu");
    if (!cJSON_IsObject(gpu))
        return;
    out->has_gpu = true;
    (void)copy_str(gpu, "name", out->gpu_name, sizeof(out->gpu_name));
    (void)opt_num(gpu, "vram_used_mb", 0.0, &out->gpu_vram_used_mb);
    (void)opt_num(gpu, "vram_total_mb", 0.0, &out->gpu_vram_total_mb);
    (void)opt_num(gpu, "compute_pct", 0.0, &out->gpu_compute_pct);
}

bool kdash_parse_telemetry(const char *json, size_t len,
                           kdash_telemetry_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_token(root, "hostname", out->hostname, sizeof(out->hostname)) &&
              req_num(root, "ts", -1e18, &out->ts) &&
              req_num(root, "cpu_pct", 0.0, &out->cpu_pct) &&
              req_num(root, "ram_used_mb", 0.0, &out->ram_used_mb) &&
              req_num(root, "ram_total_mb", 0.0, &out->ram_total_mb);

    if (ok) {
        out->has_top_core = opt_num(root, "top_core_pct", 0.0, &out->top_core_pct);
        parse_gpu(root, out);
        parse_disks(root, out);
    } else {
        memset(out, 0, sizeof(*out));
    }

    cJSON_Delete(root);
    return ok;
}

/* ---- dev_telemetry ------------------------------------------------------ */

bool kdash_parse_dev_telemetry(const char *json, size_t len,
                               kdash_dev_telemetry_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_token(root, "hostname", out->hostname, sizeof(out->hostname)) &&
              req_num(root, "ts", -1e18, &out->ts) &&
              req_num(root, "cpu_pct", 0.0, &out->cpu_pct) &&
              req_num(root, "ram_used_mb", 0.0, &out->ram_used_mb) &&
              req_num(root, "ram_total_mb", 0.0, &out->ram_total_mb);

    if (ok) {
        /* `host` routes samples to a per-host series and is optional; a
         * present-but-malformed one is dropped like any bad optional, leaving
         * "" for the caller's "(legacy)" treatment. */
        (void)req_token(root, "host", out->host, sizeof(out->host));
        out->has_top_core = opt_num(root, "top_core_pct", 0.0, &out->top_core_pct);
        out->has_gpu = opt_num(root, "gpu_compute_pct", 0.0, &out->gpu_compute_pct);
        (void)opt_num(root, "gpu_vram_used_mb", 0.0, &out->gpu_vram_used_mb);
        (void)opt_num(root, "gpu_vram_total_mb", 0.0, &out->gpu_vram_total_mb);
    } else {
        memset(out, 0, sizeof(*out));
    }

    cJSON_Delete(root);
    return ok;
}

/* ---- service status ----------------------------------------------------- */

static const struct {
    const char *word;
    kdash_service_state_t state;
} STATES[] = {
    {"ok", KDASH_SVC_OK},
    {"unhealthy", KDASH_SVC_UNHEALTHY},
    {"maintenance", KDASH_SVC_MAINTENANCE},
    {"down", KDASH_SVC_DOWN},
    {"unknown", KDASH_SVC_UNKNOWN},
};

bool kdash_service_state_from_str(const char *s, kdash_service_state_t *out) {
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(STATES) / sizeof(STATES[0]); i++) {
        if (strcmp(s, STATES[i].word) == 0) {
            *out = STATES[i].state;
            return true;
        }
    }
    return false;
}

const char *kdash_service_state_str(kdash_service_state_t s) {
    for (size_t i = 0; i < sizeof(STATES) / sizeof(STATES[0]); i++)
        if (STATES[i].state == s)
            return STATES[i].word;
    return "unknown";
}

bool kdash_parse_service(const char *json, size_t len, kdash_service_t *out) {
    if (!out)
        return false;
    /* Identity lives on the key, and the reader may have filled it already —
     * so zero only the payload half. */
    out->ts = 0;
    out->state = KDASH_SVC_OK;
    out->text[0] = '\0';
    out->has_icon = false;
    out->icon = 0;

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_num(root, "ts", -1e18, &out->ts);
    if (ok) {
        const cJSON *st = cJSON_GetObjectItemCaseSensitive(root, "state");
        ok = cJSON_IsString(st) && st->valuestring &&
             kdash_service_state_from_str(st->valuestring, &out->state);
    }
    if (ok)
        ok = copy_str(root, "text", out->text, sizeof(out->text));

    if (ok) {
        double icon = 0;
        if (opt_num(root, "icon", -1e9, &icon)) {
            out->has_icon = true;
            out->icon = (int)icon;
        }
    } else {
        out->ts = 0;
        out->state = KDASH_SVC_OK;
        out->text[0] = '\0';
        out->has_icon = false;
        out->icon = 0;
    }

    cJSON_Delete(root);
    return ok;
}

/* ---- apartment temperatures --------------------------------------------- */

bool kdash_parse_apttemps(const char *json, size_t len, kdash_apttemps_t *out) {
    if (!out)
        return false;
    out->label[0] = '\0';
    out->temp_f = out->humidity_pct = out->ts = 0;

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_num(root, "temp_f", -1e9, &out->temp_f) &&
              req_num(root, "humidity_pct", -1e9, &out->humidity_pct) &&
              req_num(root, "ts", -1e18, &out->ts);

    if (ok)
        (void)copy_str(root, "zone", out->label, sizeof(out->label));
    else
        out->temp_f = out->humidity_pct = out->ts = 0;

    cJSON_Delete(root);
    return ok;
}

/* ---- panel control ------------------------------------------------------ */

static const struct {
    const char *word;
    kdash_panel_want_t want;
} WANTS[] = {
    {"dash", KDASH_PANEL_DASH},
    {"desktop", KDASH_PANEL_DESKTOP},
};

bool kdash_panel_want_from_str(const char *s, kdash_panel_want_t *out) {
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(WANTS) / sizeof(WANTS[0]); i++) {
        if (strcmp(s, WANTS[i].word) == 0) {
            *out = WANTS[i].want;
            return true;
        }
    }
    return false;
}

const char *kdash_panel_want_str(kdash_panel_want_t w) {
    for (size_t i = 0; i < sizeof(WANTS) / sizeof(WANTS[0]); i++)
        if (WANTS[i].want == w)
            return WANTS[i].word;
    return "dash";
}

bool kdash_parse_panel(const char *json, size_t len, kdash_panel_t *out) {
    if (!out)
        return false;
    /* Identity lives on the key, and the reader may have filled it already —
     * so zero only the payload half. */
    out->want = KDASH_PANEL_DASH;
    out->ts = 0;

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    const cJSON *w = cJSON_GetObjectItemCaseSensitive(root, "want");
    bool ok = cJSON_IsString(w) && w->valuestring &&
              kdash_panel_want_from_str(w->valuestring, &out->want);
    /* Positive, not merely numeric: the stamp is this record's identity, and
     * 0 is already the family's spelling of "no usable stamp". */
    if (ok)
        ok = req_num(root, "ts", 0.0, &out->ts) && out->ts > 0.0;

    if (!ok) {
        out->want = KDASH_PANEL_DASH;
        out->ts = 0;
    }

    cJSON_Delete(root);
    return ok;
}

bool kdash_panel_actionable(const kdash_panel_t *cmd, double acted_ts,
                            long long now, long long window_s) {
    /* A zeroed record — what a rejected parse or an absent key leaves behind —
     * carries ts 0 and stops here. */
    if (!cmd || cmd->ts <= 0.0)
        return false;
    /* Strictly newer: a republish of the same command is the same command, and
     * a stamp that went backwards cannot resurrect an older one. */
    if (cmd->ts <= acted_ts)
        return false;
    /* And the window, so a restart does not replay yesterday's switch. */
    return !kdash_ts_stale(cmd->ts, now, window_s);
}

/* ---- claude: HASH field/value helpers ---------------------------------- */

/* Every value in a Redis HASH arrives as a string, so the whole "is this a
 * number" question that cJSON answers for the JSON feeds has to be asked here
 * instead — strictly, because a value that is not a number must reject rather
 * than read as zero. */

/* Strict positive integer. Rejects empty, non-numeric, trailing junk, zero and
 * negatives — every stamp in this family is a unix time, and 0 is the family's
 * own spelling of "unknown". */
static bool field_ts(const char *s, double *out) {
    if (!s || s[0] == '\0')
        return false;
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0)
        return false;
    *out = (double)v;
    return true;
}

/* Percentage: numeric and finite, clamped to [0,100] as the schemas say
 * readers do. Garbage, NaN and inf reject. */
static bool field_pct(const char *s, double *out) {
    if (!s || s[0] == '\0')
        return false;
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || !isfinite(v))
        return false;
    if (v < 0.0)
        v = 0.0;
    if (v > 100.0)
        v = 100.0;
    *out = v;
    return true;
}

/* Display string into a fixed buffer, truncating to fit. Truncation is right
 * here for exactly the reason copy_str() gives above: these are labels, not
 * identity. */
static void field_str(char *dst, size_t dstsz, const char *src) {
    snprintf(dst, dstsz, "%s", src ? src : "");
}

/* ---- claude:session ------------------------------------------------------ */

static const struct {
    const char *word;
    kdash_claude_status_t status;
} CLAUDE_STATES[] = {
    {"working", KDASH_CLAUDE_WORKING},
    {"awaiting", KDASH_CLAUDE_AWAITING},
    {"blocked", KDASH_CLAUDE_BLOCKED},
};

bool kdash_claude_status_from_str(const char *s, kdash_claude_status_t *out) {
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(CLAUDE_STATES) / sizeof(CLAUDE_STATES[0]); i++) {
        if (strcmp(s, CLAUDE_STATES[i].word) == 0) {
            *out = CLAUDE_STATES[i].status;
            return true;
        }
    }
    return false;
}

const char *kdash_claude_status_str(kdash_claude_status_t s) {
    for (size_t i = 0; i < sizeof(CLAUDE_STATES) / sizeof(CLAUDE_STATES[0]); i++)
        if (CLAUDE_STATES[i].status == s)
            return CLAUDE_STATES[i].word;
    return "working";
}

const char *kdash_claude_disp_str(kdash_claude_disp_t d) {
    switch (d) {
    case KDASH_CLAUDE_DISP_BLOCKED:
        return "blocked";
    case KDASH_CLAUDE_DISP_AWAITING:
        return "awaiting";
    case KDASH_CLAUDE_DISP_IDLE:
        return "idle";
    case KDASH_CLAUDE_DISP_STALE:
        return "stale";
    case KDASH_CLAUDE_DISP_WORKING:
    default:
        return "working";
    }
}

bool kdash_parse_claude_session(const char *host, const char *sid,
                                const char *const *fields,
                                const char *const *values, int nfields,
                                kdash_claude_session_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!host || !sid || !fields || !values || nfields <= 0)
        return false;
    /* The key was validated at discovery, but this is the parse boundary and a
     * caller may be handing back segments that took a detour through config. */
    if (!kdash_token_ok(host, strlen(host)) || !kdash_token_ok(sid, strlen(sid)))
        return false;

    kdash_claude_session_t s;
    memset(&s, 0, sizeof(s));
    field_str(s.host, sizeof(s.host), host);
    field_str(s.sid, sizeof(s.sid), sid);

    bool have_status = false, have_ts = false;
    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        const char *v = values[i];
        if (!f || !v)
            continue;
        if (strcmp(f, "status") == 0) {
            /* The schema's enum is closed: a word nobody publishes distrusts
             * the whole record rather than defaulting to `working`. */
            if (!kdash_claude_status_from_str(v, &s.status))
                return false;
            have_status = true;
        } else if (strcmp(f, "ts") == 0) {
            have_ts = field_ts(v, &s.ts);
        } else if (strcmp(f, "started_ts") == 0) {
            (void)field_ts(v, &s.started_ts);
        } else if (strcmp(f, "project") == 0) {
            field_str(s.project, sizeof(s.project), v);
        } else if (strcmp(f, "title") == 0) {
            field_str(s.title, sizeof(s.title), v);
        } else if (strcmp(f, "model") == 0) {
            field_str(s.model, sizeof(s.model), v);
        }
        /* cwd, host and every unknown field: ignored. */
    }

    /* status + ts are the liveness contract. A hash without both is either a
     * statusline resurrecting a session after SessionEnd, or a keepalive that
     * landed before the first full write — and neither is a session to show. */
    if (!have_status || !have_ts)
        return false;

    *out = s;
    return true;
}

/* ---- claude: derived display state --------------------------------------- */

kdash_claude_disp_t kdash_claude_display(kdash_claude_status_t status,
                                         long long age_s, long long idle_s,
                                         long long stale_s) {
    switch (kdash_ladder(age_s, idle_s, stale_s)) {
    case KDASH_STALE:
        /* Age has the last say: the hooks cannot report a killed process, so a
         * record still claiming `working` an hour on is claiming it about a
         * process that may not exist. */
        return KDASH_CLAUDE_DISP_STALE;
    case KDASH_IDLE:
        /* Only `working` degrades to idle. A turn does not stop being yours
         * because you took twenty minutes over it. */
        if (status == KDASH_CLAUDE_BLOCKED)
            return KDASH_CLAUDE_DISP_BLOCKED;
        if (status == KDASH_CLAUDE_AWAITING)
            return KDASH_CLAUDE_DISP_AWAITING;
        return KDASH_CLAUDE_DISP_IDLE;
    case KDASH_FRESH:
    default:
        break;
    }

    switch (status) {
    case KDASH_CLAUDE_BLOCKED:
        return KDASH_CLAUDE_DISP_BLOCKED;
    case KDASH_CLAUDE_AWAITING:
        return KDASH_CLAUDE_DISP_AWAITING;
    case KDASH_CLAUDE_WORKING:
    default:
        return KDASH_CLAUDE_DISP_WORKING;
    }
}

static int claude_session_cmp(const void *pa, const void *pb) {
    const kdash_claude_session_t *a = pa, *b = pb;
    if (a->disp != b->disp)
        return (a->disp < b->disp) ? -1 : 1;
    if (a->ts != b->ts)
        return (a->ts > b->ts) ? -1 : 1; /* most recent first */
    int h = strcmp(a->host, b->host);
    if (h != 0)
        return h;
    return strcmp(a->sid, b->sid);
}

void kdash_claude_sessions_refresh(kdash_claude_session_t *arr, int n,
                                   long long now, long long idle_s,
                                   long long stale_s) {
    if (!arr || n <= 0)
        return;
    for (int i = 0; i < n; i++)
        arr[i].disp = kdash_claude_display(arr[i].status,
                                           kdash_age_s(arr[i].ts, now), idle_s,
                                           stale_s);
    qsort(arr, (size_t)n, sizeof(arr[0]), claude_session_cmp);
}

/* ---- claude:limits ------------------------------------------------------- */

bool kdash_parse_claude_limits(const char *const *fields,
                               const char *const *values, int nfields,
                               kdash_claude_limits_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));
    if (!fields || !values || nfields <= 0)
        return false;

    kdash_claude_limits_t l;
    memset(&l, 0, sizeof(l));
    bool have_five = false, have_seven = false, have_scoped = false;

    for (int i = 0; i < nfields; i++) {
        const char *f = fields[i];
        const char *v = values[i];
        if (!f || !v)
            continue;
        if (strcmp(f, "five_hour_pct") == 0)
            have_five = field_pct(v, &l.five_hour_pct);
        else if (strcmp(f, "seven_day_pct") == 0)
            have_seven = field_pct(v, &l.seven_day_pct);
        else if (strcmp(f, "five_hour_resets_at") == 0)
            (void)field_ts(v, &l.five_hour_resets_at);
        else if (strcmp(f, "seven_day_resets_at") == 0)
            (void)field_ts(v, &l.seven_day_resets_at);
        else if (strcmp(f, "updated_at") == 0)
            (void)field_ts(v, &l.updated_at);
        else if (strcmp(f, "expected_refresh_s") == 0)
            (void)field_ts(v, &l.expected_refresh_s);
        else if (strcmp(f, "scoped_model") == 0)
            field_str(l.scoped_model, sizeof(l.scoped_model), v);
        else if (strcmp(f, "scoped_pct") == 0)
            have_scoped = field_pct(v, &l.scoped_pct);
        else if (strcmp(f, "scoped_resets_at") == 0)
            (void)field_ts(v, &l.scoped_resets_at);
        else if (strcmp(f, "scoped_active") == 0) {
            double active = 0;
            l.scoped_active = field_ts(v, &active) && active != 0;
        } else if (strcmp(f, "scoped_updated_at") == 0)
            (void)field_ts(v, &l.scoped_updated_at);
        else if (strcmp(f, "scoped_expected_refresh_s") == 0)
            (void)field_ts(v, &l.scoped_expected_refresh_s);
    }

    if (!have_five || !have_seven)
        return false;

    /* Half a scoped set renders as no scoped set at all, rather than as an
     * unlabelled gauge or a label with no number behind it. */
    l.scoped_valid = have_scoped && l.scoped_model[0] != '\0';
    l.valid = true;
    *out = l;
    return true;
}

/* Stale once a stamp is older than its own writer's cadence plus grace; a hash
 * from a pre-cadence writer published no cadence, and falls back to the fixed
 * legacy window. Skew (a stamp in the future) is never stale — same rule as
 * kdash_age_s(). */
static bool stamp_stale(double stamp, double expect_s, long long now,
                        long long grace_s, long long legacy_window_s) {
    long long window =
        (expect_s > 0) ? (long long)expect_s + grace_s : legacy_window_s;
    return kdash_age_s(stamp, now) > window;
}

bool kdash_claude_limits_stale(const kdash_claude_limits_t *l, long long now,
                               long long grace_s, long long legacy_window_s) {
    if (!l || !l->valid)
        return false;
    return stamp_stale(l->updated_at, l->expected_refresh_s, now, grace_s,
                       legacy_window_s);
}

bool kdash_claude_limits_scoped_stale(const kdash_claude_limits_t *l,
                                      long long now, long long grace_s,
                                      long long legacy_window_s) {
    if (!l || !l->scoped_valid)
        return false;
    /* A scoped set with no stamp of its own must never borrow the headline's
     * freshness — only an oauth poll can refresh these numbers, and a
     * statusline write that cannot touch them must not make them look live. */
    if (l->scoped_updated_at <= 0)
        return true;
    return stamp_stale(l->scoped_updated_at, l->scoped_expected_refresh_s, now,
                       grace_s, legacy_window_s);
}

/* ---- claude:recent ------------------------------------------------------- */

bool kdash_parse_claude_recent(const char *json, size_t len,
                               kdash_claude_recent_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    /* host is identity and rides the token contract; project is required
     * non-empty, because a recent entry with neither a project nor a title has
     * nothing to say. */
    bool ok = req_token(root, "host", out->host, sizeof(out->host)) &&
              copy_str(root, "project", out->project, sizeof(out->project)) &&
              out->project[0] != '\0';

    if (ok) {
        (void)copy_str(root, "title", out->title, sizeof(out->title));
        (void)opt_num(root, "ended_ts", 1.0, &out->ended_ts);
        (void)opt_num(root, "dur_s", 1.0, &out->dur_s);
    } else {
        memset(out, 0, sizeof(*out));
    }

    cJSON_Delete(root);
    return ok;
}
