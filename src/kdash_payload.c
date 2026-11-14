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

#include <limits.h>
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

kdash_temp_band_t kdash_apttemps_band(double temp_f, bool stale, double cold_f,
                                      double ok_f, double hot_f) {
    /* Age beats the number: an unrefreshed reading is not evidence about the
     * room, however comfortable it looks. */
    if (stale)
        return KDASH_TEMP_STALE;

    /* Clamp rather than reject, so a caller cannot configure a band that can
     * never be reached (kdash_ladder() makes the same guarantee).
     *
     * Only ONE clamp is needed, which is not obvious: `hot_f` below `ok_f`
     * corrects itself, because the `temp_f <= ok_f` test below runs first and
     * has already claimed everything a collapsed warm band could have held.
     * A second `if (hot_f < ok_f) hot_f = ok_f;` looks symmetrical and cannot
     * change any answer — it was written, found to be untestable, and removed
     * rather than left as a branch no test could ever reach. */
    if (ok_f < cold_f)
        ok_f = cold_f;

    if (temp_f < cold_f)
        return KDASH_TEMP_COLD;
    if (temp_f <= ok_f)
        return KDASH_TEMP_OK;
    if (temp_f < hot_f)
        return KDASH_TEMP_WARM;
    return KDASH_TEMP_HOT;
}

const char *kdash_temp_band_label(kdash_temp_band_t b) {
    switch (b) {
    case KDASH_TEMP_COLD:
        return "cold";
    case KDASH_TEMP_WARM:
        return "warm";
    case KDASH_TEMP_HOT:
        return "hot";
    case KDASH_TEMP_STALE:
        return "stale";
    case KDASH_TEMP_OK:
    default:
        return "ok";
    }
}

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

bool kdash_cmd_actionable(double cmd_ts, double acted_ts, long long now,
                          long long window_s) {
    /* A zeroed record — what a rejected parse or an absent key leaves behind —
     * carries ts 0 and stops here. */
    if (cmd_ts <= 0.0)
        return false;
    /* Strictly newer: a republish of the same command is the same command, and
     * a stamp that went backwards cannot resurrect an older one. */
    if (cmd_ts <= acted_ts)
        return false;
    /* And the window, so a restart does not replay yesterday's switch. */
    return !kdash_ts_stale(cmd_ts, now, window_s);
}

bool kdash_panel_actionable(const kdash_panel_t *cmd, double acted_ts,
                            long long now, long long window_s) {
    if (!cmd)
        return false;
    return kdash_cmd_actionable(cmd->ts, acted_ts, now, window_s);
}

/* ---- panel mode + screenshot commands ----------------------------------- */

/* The schema's own patterns, in C. Deliberately narrower than the host-token
 * contract: these are payload vocabulary, not key segments, and the schema is
 * the source of truth for both. A name that does not conform is rejected
 * rather than truncated into place — a truncated setting name sets the wrong
 * knob, which is the same failure kdeskdash's launcher `key` learned to
 * refuse. */
static bool pattern_ok(const char *s, size_t n, size_t maxlen, bool dash_ok) {
    if (!s || n == 0 || n > maxlen)
        return false;
    if (s[0] < 'a' || s[0] > 'z')
        return false;
    for (size_t i = 1; i < n; i++) {
        char c = s[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            continue;
        if (dash_ok && c == '-')
            continue;
        return false;
    }
    return true;
}

/* `^[a-z][a-z0-9_-]{0,30}$` */
static bool mode_ok(const char *s, size_t n) {
    return pattern_ok(s, n, KDASH_MODE_MAX - 1, true);
}

/* `^[a-z][a-z0-9_]{0,30}$` */
static bool setting_name_ok(const char *s, size_t n) {
    return pattern_ok(s, n, KDASH_SETTING_NAME_MAX - 1, false);
}

/* The optional `settings` object. Absent, or present as anything but an
 * object, is simply absent — the optional-field rule — because a mode switch
 * carrying one unreadable knob is still a mode switch. */
static void parse_settings(const cJSON *root, kdash_panelmode_t *out) {
    const cJSON *obj = cJSON_GetObjectItemCaseSensitive(root, "settings");
    if (!cJSON_IsObject(obj))
        return;

    const cJSON *e = NULL;
    cJSON_ArrayForEach(e, obj) {
        if (out->settings_count >= KDASH_SETTINGS_MAX) {
            out->settings_truncated = true;
            break;
        }
        const char *name = e->string;
        size_t nlen = name ? strlen(name) : 0;
        if (!setting_name_ok(name, nlen) || !cJSON_IsString(e) ||
            !e->valuestring) {
            out->settings_skipped++;
            continue;
        }
        size_t vlen = strlen(e->valuestring);
        if (vlen >= KDASH_SETTING_VALUE_MAX) {
            out->settings_skipped++;
            continue;
        }

        kdash_setting_t *s = &out->settings[out->settings_count];
        memcpy(s->name, name, nlen);
        s->name[nlen] = '\0';
        memcpy(s->value, e->valuestring, vlen);
        s->value[vlen] = '\0';
        out->settings_count++;
    }
}

bool kdash_parse_panelmode(const char *json, size_t len,
                           kdash_panelmode_t *out) {
    if (!out)
        return false;
    /* Identity lives on the key and the reader may have filled it already —
     * so zero only the payload half, as kdash_parse_panel() does. */
    out->mode[0] = '\0';
    out->settings_count = 0;
    out->settings_truncated = false;
    out->settings_skipped = 0;
    out->ts = 0;

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    const cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "mode");
    bool ok = cJSON_IsString(m) && m->valuestring &&
              mode_ok(m->valuestring, strlen(m->valuestring));
    if (ok) {
        size_t n = strlen(m->valuestring);
        memcpy(out->mode, m->valuestring, n);
        out->mode[n] = '\0';
        /* Positive, not merely numeric: the stamp is this record's identity. */
        ok = req_num(root, "ts", 0.0, &out->ts) && out->ts > 0.0;
    }
    if (ok)
        parse_settings(root, out);

    if (!ok) {
        out->mode[0] = '\0';
        out->ts = 0;
    }

    cJSON_Delete(root);
    return ok;
}

const char *kdash_setting_get(const kdash_panelmode_t *cmd, const char *name) {
    if (!cmd || !name)
        return NULL;
    for (int i = 0; i < cmd->settings_count; i++)
        if (strcmp(cmd->settings[i].name, name) == 0)
            return cmd->settings[i].value;
    return NULL;
}

bool kdash_parse_panelshot(const char *json, size_t len,
                           kdash_panelshot_t *out) {
    if (!out)
        return false;
    out->path[0] = '\0';
    out->ts = 0;

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_num(root, "ts", 0.0, &out->ts) && out->ts > 0.0;
    if (ok) {
        /* Optional, and absolute or nothing: a relative path would resolve
         * against whatever the dashboard's working directory happens to be,
         * so it is treated as absent and the caller keeps its default. */
        const cJSON *pth = cJSON_GetObjectItemCaseSensitive(root, "path");
        if (cJSON_IsString(pth) && pth->valuestring &&
            pth->valuestring[0] == '/') {
            size_t n = strlen(pth->valuestring);
            if (n < KDASH_PATH_MAX) {
                memcpy(out->path, pth->valuestring, n);
                out->path[n] = '\0';
            }
        }
    } else {
        out->ts = 0;
    }

    cJSON_Delete(root);
    return ok;
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
        else if (strcmp(f, "five_hour_none") == 0) {
            double none = 0;
            l.five_hour_none = field_ts(v, &none) && none == 1;
        } else if (strcmp(f, "scoped_active") == 0) {
            double active = 0;
            l.scoped_active = field_ts(v, &active) && active != 0;
        } else if (strcmp(f, "scoped_updated_at") == 0)
            (void)field_ts(v, &l.scoped_updated_at);
        else if (strcmp(f, "scoped_expected_refresh_s") == 0)
            (void)field_ts(v, &l.scoped_expected_refresh_s);
    }

    if (!have_five || !have_seven)
        return false;

    /* No five-hour window (CD-29): whatever five-hour numbers the hash still
     * holds were left by some earlier writer, and must not render. */
    if (l.five_hour_none) {
        l.five_hour_pct = 0;
        l.five_hour_resets_at = 0;
    }

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

/* ---- the status feeds (CD-28) ------------------------------------------- */

/* Upper bound on a unix-seconds stamp, the schemas' `maximum`. A millisecond
 * stamp exceeds it, which is the point: it would otherwise read as a record
 * from the far future, i.e. as permanently fresh. */
#define STAMP_MAX 1e11

/* A JSON number that is a whole number in [min, max]. `integer` in the schema
 * means exactly this; 3.5 projects is a malformed record, not 3. */
static bool whole(const cJSON *o, double min, double max, long long *out) {
    if (!cJSON_IsNumber(o))
        return false;
    double v = o->valuedouble;
    if (!(v >= min && v <= max) || floor(v) != v)
        return false;
    *out = (long long)v;
    return true;
}

/* Required whole number, small enough for an int. */
static bool req_count(const cJSON *root, const char *field, double min,
                      int *out) {
    long long v;
    if (!whole(cJSON_GetObjectItemCaseSensitive(root, field), min, INT_MAX, &v))
        return false;
    *out = (int)v;
    return true;
}

/* Required, nullable whole number: present-and-null is KDASH_COUNT_NULL,
 * ABSENT rejects (CD-27 — absence and null are different statements). */
static bool req_count_or_null(const cJSON *root, const char *field, double min,
                              long long *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (cJSON_IsNull(o)) {
        *out = KDASH_COUNT_NULL;
        return true;
    }
    return whole(o, min, 9007199254740992.0 /* 2^53 */, out);
}

/* Required, nullable unix-seconds stamp: null is 0, which is the library's
 * existing spelling for an unknown stamp. */
static bool req_stamp_or_null(const cJSON *root, const char *field,
                              double *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (cJSON_IsNull(o)) {
        *out = 0;
        return true;
    }
    if (!cJSON_IsNumber(o) || !(o->valuedouble > 0) ||
        o->valuedouble > STAMP_MAX || floor(o->valuedouble) != o->valuedouble)
        return false;
    *out = o->valuedouble;
    return true;
}

/* `ts` (positive float, bounded) and `expected_refresh_s` (whole, 1..86400):
 * the head both status feeds share. */
static bool req_status_head(const cJSON *root, double *ts, double *cadence) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(root, "ts");
    if (!cJSON_IsNumber(t) || !(t->valuedouble > 0) || t->valuedouble > STAMP_MAX)
        return false;
    long long c;
    if (!whole(cJSON_GetObjectItemCaseSensitive(root, "expected_refresh_s"), 1,
               86400, &c))
        return false;
    *ts = t->valuedouble;
    *cadence = (double)c;
    return true;
}

/* A required string of 1..outsz-1 chars, copied whole. Unlike copy_str() it
 * refuses rather than truncates: two names that differ past the cut would
 * otherwise render as one. */
static bool req_name(const cJSON *root, const char *field, char *out,
                     size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring)
        return false;
    size_t n = strlen(o->valuestring);
    if (n == 0 || n + 1 > outsz)
        return false;
    memcpy(out, o->valuestring, n + 1);
    return true;
}

/* `YYYY-MM-DD`, exactly: the schema's pattern. */
static bool req_date(const cJSON *root, const char *field,
                     char out[KDASH_DATE_MAX]) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring || strlen(o->valuestring) != 10)
        return false;
    const char *s = o->valuestring;
    for (int i = 0; i < 10; i++) {
        bool dash = (i == 4 || i == 7);
        if (dash ? s[i] != '-' : (s[i] < '0' || s[i] > '9'))
            return false;
    }
    memcpy(out, s, 11);
    return true;
}

/* Walk a required array, keeping at most `max` entries. `keep_newest` keeps
 * the LAST `max` (an oldest-first series, whose tail is today) instead of the
 * first. Each kept-range item goes to `take`, which fills slot `n` and returns
 * false for a malformed entry — skipped and counted, never fatal. Returns false
 * only when the field is absent or not an array: that rejects the record. */
static bool walk_array(const cJSON *root, const char *field, int max,
                       bool keep_newest,
                       bool (*take)(const cJSON *item, void *slots, int n),
                       void *slots, int *count, int *skipped, bool *truncated) {
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsArray(arr))
        return false;
    int size = cJSON_GetArraySize(arr);
    int skip_head = 0;
    if (size > max) {
        *truncated = true;
        if (keep_newest)
            skip_head = size - max;
    }
    int i = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr) {
        if (i++ < skip_head)
            continue;
        if (*count >= max)
            break;
        if (take(item, slots, *count))
            (*count)++;
        else
            (*skipped)++;
    }
    return true;
}

/* ---- kdash:korg:pulse ---- */

static bool take_korg_project(const cJSON *item, void *slots, int n) {
    kdash_korg_project_t *p = &((kdash_korg_project_t *)slots)[n];
    memset(p, 0, sizeof(*p));
    if (cJSON_IsObject(item) && req_name(item, "name", p->name, sizeof(p->name)) &&
        req_count(item, "open", 1, &p->open))
        return true;
    memset(p, 0, sizeof(*p));
    return false;
}

static bool take_korg_day(const cJSON *item, void *slots, int n) {
    kdash_korg_day_t *d = &((kdash_korg_day_t *)slots)[n];
    memset(d, 0, sizeof(*d));
    if (cJSON_IsObject(item) && req_date(item, "date", d->date) &&
        req_count(item, "open", 0, &d->open))
        return true;
    memset(d, 0, sizeof(*d));
    return false;
}

bool kdash_parse_korg_pulse(const char *json, size_t len,
                            kdash_korg_pulse_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    bool ok = req_status_head(root, &out->ts, &out->expected_refresh_s) &&
              req_count(root, "open", 0, &out->open) &&
              req_count(root, "projects", 0, &out->projects) &&
              req_count(root, "awaiting", 0, &out->awaiting) &&
              req_count(root, "active", 0, &out->active) &&
              req_count(root, "queued", 0, &out->queued) &&
              req_count(root, "other_open", 0, &out->other_open) &&
              walk_array(root, "by_project", KDASH_KORG_PROJECTS_MAX, false,
                         take_korg_project, out->by_project,
                         &out->by_project_count, &out->by_project_skipped,
                         &out->by_project_truncated) &&
              walk_array(root, "history", KDASH_KORG_HISTORY_MAX, true,
                         take_korg_day, out->history, &out->history_count,
                         &out->history_skipped, &out->history_truncated);

    if (!ok)
        memset(out, 0, sizeof(*out));
    cJSON_Delete(root);
    return ok;
}

/* ---- kdash:klams:status ---- */

static const struct {
    const char *word;
    kdash_klams_health_t health;
} KLAMS_HEALTH[] = {
    {"ok", KDASH_KLAMS_OK},
    {"degraded", KDASH_KLAMS_DEGRADED},
    {"down", KDASH_KLAMS_DOWN},
};

bool kdash_klams_health_from_str(const char *s, kdash_klams_health_t *out) {
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(KLAMS_HEALTH) / sizeof(KLAMS_HEALTH[0]); i++) {
        if (strcmp(s, KLAMS_HEALTH[i].word) == 0) {
            *out = KLAMS_HEALTH[i].health;
            return true;
        }
    }
    return false;
}

const char *kdash_klams_health_str(kdash_klams_health_t h) {
    for (size_t i = 0; i < sizeof(KLAMS_HEALTH) / sizeof(KLAMS_HEALTH[0]); i++)
        if (KLAMS_HEALTH[i].health == h)
            return KLAMS_HEALTH[i].word;
    return "down";
}

static bool take_klams_scanner(const cJSON *item, void *slots, int n) {
    kdash_klams_scanner_t *s = &((kdash_klams_scanner_t *)slots)[n];
    memset(s, 0, sizeof(*s));
    long long interval;
    if (cJSON_IsObject(item) && req_name(item, "name", s->name, sizeof(s->name)) &&
        req_stamp_or_null(item, "last_wrote_at", &s->last_wrote_at) &&
        req_stamp_or_null(item, "last_ran_at", &s->last_ran_at) &&
        whole(cJSON_GetObjectItemCaseSensitive(item, "expected_interval_s"), 1,
              604800, &interval)) {
        s->expected_interval_s = (double)interval;
        return true;
    }
    memset(s, 0, sizeof(*s));
    return false;
}

static bool take_klams_subsystem(const cJSON *item, void *slots, int n) {
    kdash_klams_subsystem_t *s = &((kdash_klams_subsystem_t *)slots)[n];
    memset(s, 0, sizeof(*s));
    const cJSON *ok = cJSON_IsObject(item)
                          ? cJSON_GetObjectItemCaseSensitive(item, "ok")
                          : NULL;
    if (cJSON_IsBool(ok) && req_name(item, "name", s->name, sizeof(s->name))) {
        s->ok = cJSON_IsTrue(ok);
        return true;
    }
    memset(s, 0, sizeof(*s));
    return false;
}

static bool take_klams_day(const cJSON *item, void *slots, int n) {
    kdash_klams_day_t *d = &((kdash_klams_day_t *)slots)[n];
    memset(d, 0, sizeof(*d));
    if (cJSON_IsObject(item) && req_date(item, "date", d->date) &&
        whole(cJSON_GetObjectItemCaseSensitive(item, "added"), 0,
              9007199254740992.0, &d->added))
        return true;
    memset(d, 0, sizeof(*d));
    return false;
}

/* `version`: a string of at most 31 chars, or null (""). Required either way. */
static bool req_version(const cJSON *root, char *out, size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (cJSON_IsNull(o)) {
        out[0] = '\0';
        return true;
    }
    if (!cJSON_IsString(o) || !o->valuestring ||
        strlen(o->valuestring) + 1 > outsz)
        return false;
    memcpy(out, o->valuestring, strlen(o->valuestring) + 1);
    return true;
}

bool kdash_parse_klams_status(const char *json, size_t len,
                              kdash_klams_status_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "health");
    bool ok = req_status_head(root, &out->ts, &out->expected_refresh_s) &&
              cJSON_IsString(h) && h->valuestring &&
              kdash_klams_health_from_str(h->valuestring, &out->health) &&
              req_version(root, out->version, sizeof(out->version)) &&
              req_count_or_null(root, "uptime_s", 0, &out->uptime_s) &&
              req_count_or_null(root, "knowledge", 0, &out->knowledge) &&
              req_count_or_null(root, "facts", 0, &out->facts) &&
              req_count_or_null(root, "events", 0, &out->events) &&
              req_count_or_null(root, "queue_depth", 0, &out->queue_depth) &&
              req_count_or_null(root, "queue_capacity", 1, &out->queue_capacity) &&
              req_stamp_or_null(root, "backup_at", &out->backup_at) &&
              walk_array(root, "scanners", KDASH_KLAMS_SCANNERS_MAX, false,
                         take_klams_scanner, out->scanners,
                         &out->scanners_count, &out->scanners_skipped,
                         &out->scanners_truncated) &&
              walk_array(root, "subsystems", KDASH_KLAMS_SUBSYSTEMS_MAX, false,
                         take_klams_subsystem, out->subsystems,
                         &out->subsystems_count, &out->subsystems_skipped,
                         &out->subsystems_truncated) &&
              walk_array(root, "new_per_day", KDASH_KLAMS_DAYS_MAX, true,
                         take_klams_day, out->new_per_day,
                         &out->new_per_day_count, &out->new_per_day_skipped,
                         &out->new_per_day_truncated);

    if (!ok)
        memset(out, 0, sizeof(*out));
    cJSON_Delete(root);
    return ok;
}

/* ---- kdash:calendar (CD-30) ---- */

/* A required string of 1..`chars` UTF-8 characters, counted the way the
 * schema's maxLength counts them, so a writer that truncates by characters is
 * never refused here. The buffer holds `chars` four-byte characters. */
static bool req_utf8_name(const cJSON *root, const char *field, size_t chars,
                          char *out, size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring)
        return false;
    size_t n = strlen(o->valuestring), count = 0;
    for (size_t i = 0; i < n; i++)
        if (((unsigned char)o->valuestring[i] & 0xC0) != 0x80)
            count++;
    if (count == 0 || count > chars || n + 1 > outsz)
        return false;
    memcpy(out, o->valuestring, n + 1);
    return true;
}

/* An optional tag string: absent is "" and true; present must be a string of
 * 1..`chars` characters, copied whole, or the entry is malformed. Null is not
 * absence -- the schema says an untagged date omits the field. */
static bool opt_tag(const cJSON *item, const char *field, size_t chars,
                    char *out, size_t outsz) {
    if (!cJSON_GetObjectItemCaseSensitive(item, field)) {
        out[0] = '\0';
        return true;
    }
    return req_utf8_name(item, field, chars, out, outsz);
}

static bool in_window(const char *date, const char *from, const char *to) {
    return strcmp(date, from) >= 0 && strcmp(date, to) <= 0;
}

static bool take_calendar_day(const cJSON *item, const kdash_calendar_t *cal,
                              kdash_calendar_day_t *d) {
    memset(d, 0, sizeof(*d));
    if (!cJSON_IsObject(item) || !req_date(item, "date", d->date))
        return false;
    const cJSON *off = cJSON_GetObjectItemCaseSensitive(item, "off");
    if (!cJSON_IsBool(off))
        return false;
    d->off = cJSON_IsTrue(off);
    if (!opt_tag(item, "holiday", KDASH_CALENDAR_NAME_CHARS, d->holiday,
                 sizeof(d->holiday)) ||
        !opt_tag(item, "special", KDASH_CALENDAR_NAME_CHARS, d->special,
                 sizeof(d->special)) ||
        !opt_tag(item, "short", KDASH_CALENDAR_SHORT_CHARS, d->short_label,
                 sizeof(d->short_label)))
        return false;
    /* An entry with no tag says nothing; one outside the window contradicts
     * the record's own statement of what it covers. */
    if (!d->off && d->holiday[0] == '\0' && d->special[0] == '\0')
        return false;
    return in_window(d->date, cal->from, cal->to);
}

bool kdash_parse_calendar(const char *json, size_t len, kdash_calendar_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    const cJSON *days = cJSON_GetObjectItemCaseSensitive(root, "days");
    bool ok = req_status_head(root, &out->ts, &out->expected_refresh_s) &&
              req_date(root, "from", out->from) &&
              req_date(root, "to", out->to) &&
              strcmp(out->from, out->to) <= 0 && cJSON_IsArray(days);

    if (ok) {
        /* Not walk_array: whether an entry is kept depends on the window, and
         * the cap applies to KEPT entries, so the earliest 64 good dates
         * survive even when a bad one sits among them. */
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, days) {
            kdash_calendar_day_t d;
            if (!take_calendar_day(item, out, &d)) {
                out->days_skipped++;
            } else if (out->days_count < KDASH_CALENDAR_DAYS_MAX) {
                out->days[out->days_count++] = d;
            } else {
                out->days_truncated = true;
            }
        }
    }

    if (!ok)
        memset(out, 0, sizeof(*out));
    cJSON_Delete(root);
    return ok;
}

bool kdash_calendar_covers(const kdash_calendar_t *cal, const char *date) {
    if (!cal || !date || cal->from[0] == '\0' || cal->to[0] == '\0')
        return false;
    return in_window(date, cal->from, cal->to);
}

const kdash_calendar_day_t *kdash_calendar_day(const kdash_calendar_t *cal,
                                               const char *date) {
    if (!kdash_calendar_covers(cal, date))
        return NULL;
    for (int i = 0; i < cal->days_count; i++)
        if (strcmp(cal->days[i].date, date) == 0)
            return &cal->days[i];
    return NULL;
}

/* ---- kdash:agents (CD-31) ---- */

static const struct {
    const char *word;
    kdash_agent_state_t state;
} AGENT_STATE[] = {
    {"done", KDASH_AGENT_DONE},
    {"working", KDASH_AGENT_WORKING},
    {"waiting", KDASH_AGENT_WAITING},
};

bool kdash_agent_state_from_str(const char *s, kdash_agent_state_t *out) {
    if (!s || !out)
        return false;
    for (size_t i = 0; i < sizeof(AGENT_STATE) / sizeof(AGENT_STATE[0]); i++) {
        if (strcmp(s, AGENT_STATE[i].word) == 0) {
            *out = AGENT_STATE[i].state;
            return true;
        }
    }
    return false;
}

const char *kdash_agent_state_str(kdash_agent_state_t s) {
    for (size_t i = 0; i < sizeof(AGENT_STATE) / sizeof(AGENT_STATE[0]); i++)
        if (AGENT_STATE[i].state == s)
            return AGENT_STATE[i].word;
    return "done";
}

/* A token-grammar string ([A-Za-z0-9._-]) of 1..outsz-1 chars, copied whole:
 * `id`, `parent` and `host` are identity on this feed, never truncated. */
static bool req_short_token(const cJSON *root, const char *field, char *out,
                            size_t outsz) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsString(o) || !o->valuestring)
        return false;
    size_t n = strlen(o->valuestring);
    if (n + 1 > outsz || !kdash_token_ok(o->valuestring, n))
        return false;
    memcpy(out, o->valuestring, n + 1);
    return true;
}

/* A required unix-seconds stamp, positive and bounded (STAMP_MAX). */
static bool req_stamp(const cJSON *root, const char *field, double *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsNumber(o) || !(o->valuedouble > 0) ||
        o->valuedouble > STAMP_MAX)
        return false;
    *out = o->valuedouble;
    return true;
}

static bool req_bool(const cJSON *root, const char *field, bool *out) {
    const cJSON *o = cJSON_GetObjectItemCaseSensitive(root, field);
    if (!cJSON_IsBool(o))
        return false;
    *out = cJSON_IsTrue(o);
    return true;
}

/* Fill `b` from one wire bar. The parent link is copied raw and resolved
 * afterwards, because it may name a bar later in the record; a malformed one
 * is marked by `*parent_bad` rather than skipping the bar. */
static bool take_agent_bar(const cJSON *item, const kdash_agents_t *ag,
                           kdash_agent_bar_t *b, bool *parent_bad) {
    memset(b, 0, sizeof(*b));
    b->parent_index = -1;
    *parent_bad = false;
    const cJSON *state = cJSON_IsObject(item)
                             ? cJSON_GetObjectItemCaseSensitive(item, "state")
                             : NULL;
    if (!cJSON_IsString(state) ||
        !kdash_agent_state_from_str(state->valuestring, &b->state) ||
        !req_short_token(item, "id", b->id, sizeof(b->id)) ||
        !req_short_token(item, "host", b->host, sizeof(b->host)) ||
        !req_utf8_name(item, "name", KDASH_AGENTS_NAME_CHARS, b->name,
                       sizeof(b->name)) ||
        !req_stamp(item, "start", &b->start) ||
        !req_stamp(item, "end", &b->end) ||
        !req_bool(item, "shipped", &b->shipped) ||
        !req_bool(item, "overseer", &b->overseer))
        return false;
    /* A stretch that ends before it starts says nothing; one that ended
     * before the window opened is outside what the record claims to cover. */
    if (b->start > b->end || b->end < ag->ts - ag->window_s)
        return false;
    if (cJSON_GetObjectItemCaseSensitive(item, "parent") &&
        !req_short_token(item, "parent", b->parent, sizeof(b->parent))) {
        b->parent[0] = '\0';
        *parent_bad = true;
    }
    return true;
}

static int agent_bar_find(const kdash_agents_t *ag, const char *id) {
    for (int i = 0; i < ag->bars_count; i++)
        if (strcmp(ag->bars[i].id, id) == 0)
            return i;
    return -1;
}

bool kdash_parse_agents(const char *json, size_t len, kdash_agents_t *out) {
    if (!out)
        return false;
    memset(out, 0, sizeof(*out));

    cJSON *root = parse_object(json, len);
    if (!root)
        return false;

    const cJSON *bars = cJSON_GetObjectItemCaseSensitive(root, "bars");
    long long window;
    bool ok = req_status_head(root, &out->ts, &out->expected_refresh_s) &&
              whole(cJSON_GetObjectItemCaseSensitive(root, "window_s"), 3600,
                    604800, &window) &&
              cJSON_IsArray(bars);

    if (ok) {
        out->window_s = (double)window;
        /* Not walk_array: a bar's fate depends on the window and on the ids
         * already kept, and the cap applies to KEPT bars. Past the cap the
         * oldest kept bar is evicted, so the newest 96 survive -- the card's
         * right edge is now. A malformed parent counts once, at the end, only
         * if its bar survives. */
        bool parent_bad[KDASH_AGENTS_BARS_MAX] = {false}; /* parallel to bars */
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, bars) {
            kdash_agent_bar_t b;
            bool bad;
            if (!take_agent_bar(item, out, &b, &bad) ||
                agent_bar_find(out, b.id) >= 0) {
                out->bars_skipped++;
                continue;
            }
            if (out->bars_count == KDASH_AGENTS_BARS_MAX) {
                memmove(&out->bars[0], &out->bars[1],
                        (KDASH_AGENTS_BARS_MAX - 1) * sizeof(out->bars[0]));
                memmove(&parent_bad[0], &parent_bad[1],
                        (KDASH_AGENTS_BARS_MAX - 1) * sizeof(parent_bad[0]));
                out->bars_count--;
                out->bars_truncated = true;
            }
            parent_bad[out->bars_count] = bad;
            out->bars[out->bars_count++] = b;
        }

        /* Resolve links among the bars that survived. */
        for (int i = 0; i < out->bars_count; i++) {
            kdash_agent_bar_t *b = &out->bars[i];
            if (parent_bad[i]) {
                out->parents_dropped++;
                continue;
            }
            if (b->parent[0] == '\0')
                continue;
            int p = agent_bar_find(out, b->parent);
            if (p >= 0 && p != i && out->bars[p].overseer) {
                b->parent_index = p;
            } else {
                b->parent[0] = '\0';
                out->parents_dropped++;
            }
        }
    }

    if (!ok)
        memset(out, 0, sizeof(*out));
    cJSON_Delete(root);
    return ok;
}

double kdash_agents_bar_end(const kdash_agents_t *a, const kdash_agent_bar_t *b,
                            long long now) {
    if (!a || !b)
        return 0;
    double n = (double)now;
    double end = b->end < n ? b->end : n;
    if (b->state == KDASH_AGENT_DONE)
        return end;
    return kdash_cadence_stale(a->ts, a->expected_refresh_s, now,
                               KDASH_CADENCE_GRACE_S)
               ? end
               : n;
}
