/**
 * @file kdash_payload.c
 * Pure payload parsers for the five schema'd kpidash feeds (cJSON only — no
 * Redis, no sockets). Host-testable.
 *
 * Every parser follows the same shape, which is the rules.md contract:
 * zero the output, parse, take the required fields (rejecting the record
 * whole if any is missing/mistyped/out of range), then take the optional
 * fields best-effort. Unknown fields are never looked at, which is exactly
 * how a reader stays additive-evolution-safe.
 */
#include "kdash/kdash_payload.h"

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
