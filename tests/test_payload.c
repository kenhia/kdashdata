/**
 * @file test_payload.c
 * The five payload parsers against their schema files: what each one requires,
 * what it tolerates, and the additive-evolution rule that a reader which
 * rejects unknown fields is the broken half.
 */
#include <string.h>

#include "check.h"
#include "kdash/kdash_payload.h"

#define PARSE(fn, json, out) fn((json), strlen(json), (out))

int main(void) {
    /* ---- health ---- */
    {
        kdash_health_t h;
        CHECK(PARSE(kdash_parse_health,
                    "{\"hostname\":\"kai\",\"last_seen_ts\":1756600000.5,"
                    "\"uptime_seconds\":3600,\"os_name\":\"Debian GNU/Linux 13\"}",
                    &h),
              "full health payload");
        CHECK(strcmp(h.hostname, "kai") == 0, "hostname \"%s\"", h.hostname);
        CHECK(h.last_seen_ts > 1756599999.0, "last_seen_ts carried");
        CHECK(h.has_uptime && h.uptime_seconds == 3600, "uptime");
        CHECK(strcmp(h.os_name, "Debian GNU/Linux 13") == 0, "os_name");

        /* Only hostname and last_seen_ts are required. */
        CHECK(PARSE(kdash_parse_health,
                    "{\"hostname\":\"kai\",\"last_seen_ts\":1}", &h),
              "minimal health payload");
        CHECK(!h.has_uptime, "absent uptime is absent, not zero-as-a-value");
        CHECK(h.os_name[0] == '\0', "absent os_name is empty");

        /* Unknown fields are the additive-evolution contract. */
        CHECK(PARSE(kdash_parse_health,
                    "{\"hostname\":\"kai\",\"last_seen_ts\":1,\"invented\":42,"
                    "\"nested\":{\"a\":[1,2]}}",
                    &h),
              "unknown fields must be ignored, never rejected");

        /* A required field missing or mistyped rejects the record whole. */
        CHECK(!PARSE(kdash_parse_health, "{\"last_seen_ts\":1}", &h),
              "missing hostname");
        CHECK(!PARSE(kdash_parse_health, "{\"hostname\":\"kai\"}", &h),
              "missing last_seen_ts");
        CHECK(!PARSE(kdash_parse_health,
                     "{\"hostname\":\"kai\",\"last_seen_ts\":\"soon\"}", &h),
              "last_seen_ts must be numeric");
        /* hostname carries the token contract, because it reaches key
         * construction — a hostname with a colon in it is a forged key. */
        CHECK(!PARSE(kdash_parse_health,
                     "{\"hostname\":\"a:b\",\"last_seen_ts\":1}", &h),
              "hostname must satisfy the token contract");
        CHECK(!PARSE(kdash_parse_health, "not json at all", &h), "malformed JSON");
        CHECK(!PARSE(kdash_parse_health, "[1,2,3]", &h), "root must be an object");
        CHECK(!PARSE(kdash_parse_health, "", &h), "empty input");

        /* A rejected record is fully zeroed, never half-filled. */
        CHECK(h.hostname[0] == '\0' && h.last_seen_ts == 0,
              "rejected record is zeroed");

        /* An optional field that is malformed costs only itself. */
        CHECK(PARSE(kdash_parse_health,
                    "{\"hostname\":\"kai\",\"last_seen_ts\":1,"
                    "\"uptime_seconds\":\"lots\"}",
                    &h),
              "bad optional must not reject the record");
        CHECK(!h.has_uptime, "bad optional reads as absent");
    }

    /* ---- telemetry ---- */
    {
        kdash_telemetry_t t;
        CHECK(PARSE(kdash_parse_telemetry,
                    "{\"hostname\":\"kai\",\"ts\":1756600000,\"cpu_pct\":12.5,"
                    "\"top_core_pct\":44,\"ram_used_mb\":8192,"
                    "\"ram_total_mb\":32768,"
                    "\"gpu\":{\"name\":\"RTX 5090\",\"vram_used_mb\":2048,"
                    "\"vram_total_mb\":32768,\"compute_pct\":7},"
                    "\"disks\":[{\"label\":\"/\",\"type\":\"nvme\","
                    "\"used_gb\":100,\"total_gb\":900,\"pct\":11.1}]}",
                    &t),
              "full telemetry payload");
        CHECK(t.has_top_core && t.top_core_pct == 44, "top_core_pct");
        CHECK(t.has_gpu && strcmp(t.gpu_name, "RTX 5090") == 0, "gpu object");
        CHECK(t.gpu_compute_pct == 7, "gpu compute");
        CHECK(t.ndisks == 1 && t.disks_skipped == 0, "one disk");
        CHECK(strcmp(t.disks[0].label, "/") == 0 && t.disks[0].pct == 11.1,
              "disk fields");

        /* "gpu": null is how a GPU-less host spells it, and it is not an
         * error — it is the answer. */
        CHECK(PARSE(kdash_parse_telemetry,
                    "{\"hostname\":\"rpi53\",\"ts\":1,\"cpu_pct\":3,"
                    "\"ram_used_mb\":1,\"ram_total_mb\":8000,\"gpu\":null}",
                    &t),
              "gpu null parses");
        CHECK(!t.has_gpu, "gpu null means no gpu");
        CHECK(t.ndisks == 0, "absent disks array");

        /* A malformed disk entry is skipped and counted; the sample survives.
         * One bad row must not cost the whole card. */
        CHECK(PARSE(kdash_parse_telemetry,
                    "{\"hostname\":\"kai\",\"ts\":1,\"cpu_pct\":1,"
                    "\"ram_used_mb\":1,\"ram_total_mb\":2,"
                    "\"disks\":[{\"label\":\"/\",\"used_gb\":1,\"total_gb\":2,"
                    "\"pct\":50},{\"label\":\"/boot\"},"
                    "{\"label\":\"/x\",\"used_gb\":1,\"total_gb\":2,"
                    "\"pct\":150}]}",
                    &t),
              "partial disks array");
        CHECK(t.ndisks == 1, "one good disk, got %d", t.ndisks);
        CHECK(t.disks_skipped == 2, "two skipped, got %d", t.disks_skipped);

        /* Every required field, one at a time. */
        CHECK(!PARSE(kdash_parse_telemetry,
                     "{\"ts\":1,\"cpu_pct\":1,\"ram_used_mb\":1,"
                     "\"ram_total_mb\":2}",
                     &t),
              "missing hostname");
        CHECK(!PARSE(kdash_parse_telemetry,
                     "{\"hostname\":\"kai\",\"cpu_pct\":1,\"ram_used_mb\":1,"
                     "\"ram_total_mb\":2}",
                     &t),
              "missing ts");
        CHECK(!PARSE(kdash_parse_telemetry,
                     "{\"hostname\":\"kai\",\"ts\":1,\"ram_used_mb\":1,"
                     "\"ram_total_mb\":2}",
                     &t),
              "missing cpu_pct");
        /* The schema says `minimum: 0`; a negative required value is a
         * schema violation, so the record goes, not just the field. */
        CHECK(!PARSE(kdash_parse_telemetry,
                     "{\"hostname\":\"kai\",\"ts\":1,\"cpu_pct\":-1,"
                     "\"ram_used_mb\":1,\"ram_total_mb\":2}",
                     &t),
              "cpu_pct below its schema minimum");
    }

    /* ---- dev_telemetry ---- */
    {
        kdash_dev_telemetry_t d;
        CHECK(PARSE(kdash_parse_dev_telemetry,
                    "{\"hostname\":\"kai\",\"host\":\"kai\",\"ts\":1,"
                    "\"cpu_pct\":50,\"top_core_pct\":90,\"ram_used_mb\":1,"
                    "\"ram_total_mb\":2,\"gpu_compute_pct\":30,"
                    "\"gpu_vram_used_mb\":100,\"gpu_vram_total_mb\":200}",
                    &d),
              "full dev_telemetry payload");
        CHECK(strcmp(d.host, "kai") == 0, "host routing field");
        CHECK(d.has_gpu && d.gpu_compute_pct == 30, "gpu present");

        /* `host` is optional — the schema says readers treat absence as
         * "(legacy)", which is a rendering decision, so the parser leaves it
         * empty rather than inventing one. */
        CHECK(PARSE(kdash_parse_dev_telemetry,
                    "{\"hostname\":\"kai\",\"ts\":1,\"cpu_pct\":1,"
                    "\"ram_used_mb\":1,\"ram_total_mb\":2}",
                    &d),
              "dev_telemetry without host");
        CHECK(d.host[0] == '\0', "absent host stays empty");
        CHECK(!d.has_gpu, "no gpu fields means no gpu");
        CHECK(!d.has_top_core, "absent top_core_pct");
    }

    /* ---- service status ---- */
    {
        kdash_service_t s;
        memset(&s, 0, sizeof(s));
        strcpy(s.name, "korg");
        strcpy(s.host, "kubs0");
        CHECK(PARSE(kdash_parse_service,
                    "{\"ts\":1756600000,\"state\":\"ok\",\"text\":\"3 open\","
                    "\"host\":\"IGNORED\",\"icon\":7}",
                    &s),
              "full service payload");
        CHECK(s.state == KDASH_SVC_OK, "state ok");
        CHECK(strcmp(s.text, "3 open") == 0, "text");
        CHECK(s.has_icon && s.icon == 7, "icon");
        /* Identity is the key's, and the payload echo must not overwrite it. */
        CHECK(strcmp(s.name, "korg") == 0 && strcmp(s.host, "kubs0") == 0,
              "payload host echo must not overwrite key identity");

        for (int i = 0; i < 5; i++) {
            static const char *words[] = {"ok", "unhealthy", "maintenance",
                                          "down", "unknown"};
            char json[96];
            snprintf(json, sizeof(json),
                     "{\"ts\":1,\"state\":\"%s\",\"text\":\"x\"}", words[i]);
            CHECK(PARSE(kdash_parse_service, json, &s), "state %s", words[i]);
            CHECK(strcmp(kdash_service_state_str(s.state), words[i]) == 0,
                  "state %s round-trips", words[i]);
        }

        /* The schema's enum is closed. An unrecognised word is a rejected
         * record, not a silent downgrade to "unknown" — a card that renders
         * "unknown" for a state the writer meant as "down" is worse than one
         * that renders nothing. */
        CHECK(!PARSE(kdash_parse_service,
                     "{\"ts\":1,\"state\":\"degraded\",\"text\":\"x\"}", &s),
              "state outside the enum");
        CHECK(!PARSE(kdash_parse_service, "{\"state\":\"ok\",\"text\":\"x\"}", &s),
              "missing ts");
        CHECK(!PARSE(kdash_parse_service, "{\"ts\":1,\"text\":\"x\"}", &s),
              "missing state");
        CHECK(!PARSE(kdash_parse_service, "{\"ts\":1,\"state\":\"ok\"}", &s),
              "missing text");
    }

    /* ---- apttemps ---- */
    {
        kdash_apttemps_t a;
        memset(&a, 0, sizeof(a));
        strcpy(a.zone, "office");
        CHECK(PARSE(kdash_parse_apttemps,
                    "{\"zone\":\"Office\",\"temp_f\":71.4,"
                    "\"humidity_pct\":38,\"ts\":1756600000}",
                    &a),
              "full apttemps payload");
        CHECK(strcmp(a.zone, "office") == 0, "key zone stays authoritative");
        CHECK(strcmp(a.label, "Office") == 0, "payload zone is the display label");
        CHECK(a.temp_f == 71.4 && a.humidity_pct == 38, "readings");

        CHECK(PARSE(kdash_parse_apttemps,
                    "{\"temp_f\":71.4,\"humidity_pct\":38,\"ts\":1}", &a),
              "label is optional");
        CHECK(a.label[0] == '\0', "absent label is empty");
        CHECK(!PARSE(kdash_parse_apttemps, "{\"humidity_pct\":38,\"ts\":1}", &a),
              "missing temp_f");
        /* A freezing apartment is a reading, not a schema violation — temp_f
         * has no minimum, and clamping it would hide a real problem. */
        CHECK(PARSE(kdash_parse_apttemps,
                    "{\"temp_f\":-4,\"humidity_pct\":38,\"ts\":1}", &a),
              "negative temperature is legal");
    }

    /* Buffers are counted, not NUL-terminated: SCAN and GET both hand back
     * lengths, and an embedded NUL must not truncate the parse. */
    {
        kdash_health_t h;
        const char json[] = "{\"hostname\":\"kai\",\"last_seen_ts\":1}TRAILING";
        CHECK(kdash_parse_health(json, 36, &h), "length-bounded parse");
        CHECK(strcmp(h.hostname, "kai") == 0, "parsed under a length bound");
    }

    return TEST_RESULT();
}
