/**
 * @file kdash_payload.h
 * Pure parsers for the five schema'd kpidash payloads. No Redis, no sockets.
 * Host-testable — every parser takes a buffer and a length.
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

#include "kdash/kdash_keys.h"

#define KDASH_OS_NAME_MAX  64
#define KDASH_GPU_NAME_MAX 48
#define KDASH_TEXT_MAX     128
#define KDASH_LABEL_MAX    32
#define KDASH_DISK_TYPE_MAX 8

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

#endif /* KDASH_PAYLOAD_H */
