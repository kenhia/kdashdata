/**
 * @file test_payload.c
 * The payload parsers against their schema files: what each one requires, what
 * it tolerates, and the additive-evolution rule that a reader which rejects
 * unknown fields is the broken half.
 *
 * The claude half also covers the derived model — the display ladder, the
 * attention-first ordering and the per-gauge limits staleness — because those
 * are the parts of this family that two dashboards would otherwise each get
 * subtly differently.
 */
#include <stdio.h>
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

    /* ---- kdash:panel ---- */
    {
        kdash_panel_t p;
        memset(&p, 0, sizeof(p));
        strcpy(p.host, "kstudio");
        CHECK(PARSE(kdash_parse_panel, "{\"want\":\"desktop\",\"ts\":1756600000}",
                    &p),
              "full panel command");
        CHECK(p.want == KDASH_PANEL_DESKTOP, "want");
        CHECK(p.ts == 1756600000.0, "ts");
        CHECK(strcmp(p.host, "kstudio") == 0, "key host stays authoritative");

        CHECK(PARSE(kdash_parse_panel, "{\"want\":\"dash\",\"ts\":1}", &p),
              "the other word");
        CHECK(p.want == KDASH_PANEL_DASH, "want");

        /* The enum is closed. Defaulting an unknown word to `dash` would send a
         * panel back to the dashboard on a typo — the exact class of bug the
         * claude `status` enum is closed to avoid. */
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"desk\",\"ts\":1}", &p),
              "an unrecognised want rejects the record");
        CHECK(p.ts == 0, "a rejected record leaves no usable stamp");
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"DESKTOP\",\"ts\":1}", &p),
              "the enum is lowercase, and matching is exact");
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":1,\"ts\":1}", &p),
              "want must be a string");
        CHECK(!PARSE(kdash_parse_panel, "{\"ts\":1}", &p), "missing want");

        /* `ts` is this record's identity, so it is required AND positive — a
         * stamp a consumer cannot compare is no command at all. */
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"desktop\"}", &p),
              "missing ts");
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"desktop\",\"ts\":0}", &p),
              "ts 0 is the family's spelling of \"no stamp\"");
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"desktop\",\"ts\":-5}", &p),
              "a negative ts is not a command");
        CHECK(!PARSE(kdash_parse_panel, "{\"want\":\"desktop\",\"ts\":\"soon\"}",
                     &p),
              "a non-numeric ts is not a command");
        CHECK(!PARSE(kdash_parse_panel, "[\"desktop\"]", &p), "not an object");

        CHECK(PARSE(kdash_parse_panel,
                    "{\"want\":\"desktop\",\"ts\":1,\"by\":\"kdeskdash\"}", &p),
              "unknown fields must be ignored, never rejected");
    }

    /* ---- kdash:panel edge detection ---- */
    {
        const long long window = KDASH_PANEL_WINDOW_S;
        const long long now = 1756600000;
        kdash_panel_t p;
        memset(&p, 0, sizeof(p));
        p.want = KDASH_PANEL_DESKTOP;
        p.ts = (double)now - 2;

        CHECK(kdash_panel_actionable(&p, 0, now, window),
              "a fresh command nobody has acted on is actionable");

        /* Edge-triggered, not level-triggered: this is what stops the panel
         * fighting a human at the keyboard. */
        CHECK(!kdash_panel_actionable(&p, p.ts, now, window),
              "the same command must not be acted on twice");
        CHECK(!kdash_panel_actionable(&p, p.ts + 1, now, window),
              "a stamp older than the last acted-on one is not a new command");

        /* The window: a restart must not replay yesterday's switch. */
        {
            kdash_panel_t old = p;
            old.ts = (double)now - window - 1;
            CHECK(!kdash_panel_actionable(&old, 0, now, window),
                  "a command older than the window is never replayed");
            old.ts = (double)now - window + 1;
            CHECK(kdash_panel_actionable(&old, 0, now, window),
                  "and one inside it still is");
        }

        /* Writer-clock skew reads as fresh, per rules.md. */
        {
            kdash_panel_t future = p;
            future.ts = (double)now + 30;
            CHECK(kdash_panel_actionable(&future, 0, now, window),
                  "a stamp in the future is not stale");
        }

        /* The safety property the KDASH_ABSENT path leans on. */
        {
            kdash_panel_t zeroed;
            memset(&zeroed, 0, sizeof(zeroed));
            CHECK(!kdash_panel_actionable(&zeroed, 0, now, window),
                  "a zeroed record is never actionable");
            CHECK(!kdash_panel_actionable(NULL, 0, now, window), "NULL is safe");
        }
    }

    /* ---- kdash:panelmode ---- */
    {
        kdash_panelmode_t m;
        memset(&m, 0, sizeof(m));
        strcpy(m.host, "rpidash2");
        CHECK(PARSE(kdash_parse_panelmode,
                    "{\"mode\":\"golz\",\"settings\":{\"density\":\"0.35\","
                    "\"speed_ms\":\"40\"},\"ts\":1756600000}",
                    &m),
              "full mode command");
        CHECK(strcmp(m.mode, "golz") == 0, "mode \"%s\"", m.mode);
        CHECK(m.ts == 1756600000.0, "ts");
        CHECK(strcmp(m.host, "rpidash2") == 0, "key host stays authoritative");
        CHECK(m.settings_count == 2, "two settings, got %d", m.settings_count);
        CHECK(!m.settings_truncated && m.settings_skipped == 0, "nothing lost");

        /* Absent means "keep what the mode would have used" — the whole point
         * of the apply-the-fields-present injection this replaced. */
        CHECK(kdash_setting_get(&m, "density") &&
                  strcmp(kdash_setting_get(&m, "density"), "0.35") == 0,
              "a setting is looked up by name");
        CHECK(kdash_setting_get(&m, "trail") == NULL,
              "a setting the command did not carry is NULL, not empty");
        CHECK(kdash_setting_get(NULL, "density") == NULL, "NULL is safe");

        /* Settings are optional; a switch that changes nothing is legal. */
        CHECK(PARSE(kdash_parse_panelmode, "{\"mode\":\"clock\",\"ts\":1}", &m),
              "mode alone is a command");
        CHECK(m.settings_count == 0, "no settings is not an error");

        /* An optional field that is malformed is treated as ABSENT: a mode
         * switch carrying one unreadable knob is still a mode switch. */
        CHECK(PARSE(kdash_parse_panelmode,
                    "{\"mode\":\"clock\",\"settings\":7,\"ts\":1}", &m),
              "a settings that is not an object does not cost the command");
        CHECK(m.settings_count == 0, "and carries nothing");

        /* Per-entry shape. A bad entry is skipped and counted, never
         * truncated into place — a truncated name sets the wrong knob. */
        CHECK(PARSE(kdash_parse_panelmode,
                    "{\"mode\":\"gol\",\"settings\":{\"density\":\"0.2\","
                    "\"Trail\":\"1\",\"speed-ms\":\"40\",\"rgb\":true},"
                    "\"ts\":1}",
                    &m),
              "a command with three bad settings still stands");
        CHECK(m.settings_count == 1, "one good setting, got %d",
              m.settings_count);
        CHECK(m.settings_skipped == 3, "three skipped, got %d",
              m.settings_skipped);
        CHECK(kdash_setting_get(&m, "rgb") == NULL,
              "a non-string value is not a setting");

        {
            /* 63 chars is the schema's limit; 64 is rejected rather than
             * silently shortened. */
            char json[256];
            char v63[64], v64[65];
            memset(v63, 'x', 63); v63[63] = '\0';
            memset(v64, 'x', 64); v64[64] = '\0';
            snprintf(json, sizeof(json),
                     "{\"mode\":\"gol\",\"settings\":{\"a\":\"%s\"},\"ts\":1}",
                     v63);
            CHECK(PARSE(kdash_parse_panelmode, json, &m), "63-char value");
            CHECK(m.settings_count == 1, "kept");
            snprintf(json, sizeof(json),
                     "{\"mode\":\"gol\",\"settings\":{\"a\":\"%s\"},\"ts\":1}",
                     v64);
            CHECK(PARSE(kdash_parse_panelmode, json, &m), "64-char value");
            CHECK(m.settings_count == 0 && m.settings_skipped == 1,
                  "an oversize value is rejected, never truncated");
        }

        {
            /* More settings than the record holds says so rather than
             * quietly keeping the first N. */
            char json[1024];
            int n = snprintf(json, sizeof(json), "{\"mode\":\"gol\",\"settings\":{");
            for (int i = 0; i < KDASH_SETTINGS_MAX + 2; i++)
                n += snprintf(json + n, sizeof(json) - (size_t)n,
                              "%s\"s%02d\":\"1\"", i ? "," : "", i);
            snprintf(json + n, sizeof(json) - (size_t)n, "},\"ts\":1}");
            CHECK(PARSE(kdash_parse_panelmode, json, &m), "oversized settings");
            CHECK(m.settings_count == KDASH_SETTINGS_MAX, "capped at the max");
            CHECK(m.settings_truncated, "and says it was truncated");
        }

        /* `mode` is the dashboard's vocabulary, but its SHAPE is the
         * contract's: lowercase token, 1..31 chars, starting with a letter. */
        CHECK(PARSE(kdash_parse_panelmode, "{\"mode\":\"a-b_c9\",\"ts\":1}", &m),
              "the full character set");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"GOLZ\",\"ts\":1}", &m),
              "mode is lowercase");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"9lives\",\"ts\":1}", &m),
              "mode starts with a letter");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"\",\"ts\":1}", &m),
              "an empty mode is no command");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"go lz\",\"ts\":1}", &m),
              "no spaces");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":7,\"ts\":1}", &m),
              "mode must be a string");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"ts\":1}", &m), "missing mode");
        {
            char json[128];
            char m31[32], m32[33];
            memset(m31, 'a', 31); m31[31] = '\0';
            memset(m32, 'a', 32); m32[32] = '\0';
            snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"ts\":1}", m31);
            CHECK(PARSE(kdash_parse_panelmode, json, &m), "31 chars fits");
            snprintf(json, sizeof(json), "{\"mode\":\"%s\",\"ts\":1}", m32);
            CHECK(!PARSE(kdash_parse_panelmode, json, &m),
                  "32 is rejected, not truncated into a mode nobody named");
        }

        /* `ts` carries CD-17's identity rule unchanged. */
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"clock\"}", &m),
              "missing ts");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"clock\",\"ts\":0}", &m),
              "ts 0 is no stamp");
        CHECK(m.mode[0] == '\0', "a rejected record leaves no usable mode");
        CHECK(!PARSE(kdash_parse_panelmode, "{\"mode\":\"clock\",\"ts\":-5}", &m),
              "a negative ts is not a command");
        CHECK(!PARSE(kdash_parse_panelmode, "[\"clock\"]", &m), "not an object");

        CHECK(PARSE(kdash_parse_panelmode,
                    "{\"mode\":\"clock\",\"ts\":1,\"by\":\"ken\"}", &m),
              "unknown fields must be ignored, never rejected");
    }

    /* ---- kdash:panelshot ---- */
    {
        kdash_panelshot_t s;
        memset(&s, 0, sizeof(s));
        strcpy(s.host, "rpidash2");
        CHECK(PARSE(kdash_parse_panelshot,
                    "{\"path\":\"/var/tmp/kdd.bmp\",\"ts\":1756600000}", &s),
              "full screenshot command");
        CHECK(strcmp(s.path, "/var/tmp/kdd.bmp") == 0, "path \"%s\"", s.path);
        CHECK(s.ts == 1756600000.0, "ts");
        CHECK(strcmp(s.host, "rpidash2") == 0, "key host stays authoritative");

        /* The path is optional — absent means the panel's own default, which
         * is the ordinary case. */
        CHECK(PARSE(kdash_parse_panelshot, "{\"ts\":1}", &s), "ts alone");
        CHECK(s.path[0] == '\0', "absent path is empty, not a guess");

        /* Absolute or absent. A relative path would resolve against whatever
         * the dashboard's working directory happens to be. */
        CHECK(PARSE(kdash_parse_panelshot, "{\"path\":\"kdd.bmp\",\"ts\":1}", &s),
              "a relative path does not cost the capture");
        CHECK(s.path[0] == '\0', "it is simply absent");
        CHECK(PARSE(kdash_parse_panelshot, "{\"path\":7,\"ts\":1}", &s),
              "a non-string path is absent too");
        CHECK(s.path[0] == '\0', "and carries nothing");

        {
            char json[512];
            char long_path[KDASH_PATH_MAX + 8];
            long_path[0] = '/';
            memset(long_path + 1, 'p', KDASH_PATH_MAX);
            long_path[KDASH_PATH_MAX + 1] = '\0'; /* 256 chars: one too many */
            snprintf(json, sizeof(json), "{\"path\":\"%s\",\"ts\":1}", long_path);
            CHECK(PARSE(kdash_parse_panelshot, json, &s), "oversize path");
            CHECK(s.path[0] == '\0',
                  "an oversize path is dropped, never truncated to a real one");
        }

        CHECK(!PARSE(kdash_parse_panelshot, "{\"path\":\"/tmp/a.bmp\"}", &s),
              "missing ts");
        CHECK(!PARSE(kdash_parse_panelshot, "{\"ts\":0}", &s), "ts 0 is no stamp");
        CHECK(!PARSE(kdash_parse_panelshot, "{\"ts\":\"now\"}", &s),
              "a non-numeric ts is not a command");
        CHECK(!PARSE(kdash_parse_panelshot, "[]", &s), "not an object");
        CHECK(PARSE(kdash_parse_panelshot, "{\"ts\":1,\"why\":\"kddss\"}", &s),
              "unknown fields must be ignored, never rejected");
    }

    /* ---- the edge rule, family-agnostic ---- */
    {
        const long long window = KDASH_PANEL_WINDOW_S;
        const long long now = 1756600000;
        const double cmd = (double)now - 2;

        CHECK(kdash_cmd_actionable(cmd, 0, now, window),
              "a fresh command nobody has acted on is actionable");
        CHECK(!kdash_cmd_actionable(cmd, cmd, now, window),
              "the same command must not be acted on twice");
        CHECK(!kdash_cmd_actionable(0, 0, now, window),
              "a zeroed record is never actionable");
        CHECK(!kdash_cmd_actionable((double)now - window - 1, 0, now, window),
              "a command older than the window is never replayed");
        CHECK(kdash_cmd_actionable((double)now + 30, 0, now, window),
              "a stamp in the future is not stale");

        /* kdash_panel_actionable() is this function with the record
         * unwrapped, so the two must agree on every input. */
        {
            kdash_panel_t p;
            memset(&p, 0, sizeof(p));
            p.ts = cmd;
            CHECK(kdash_panel_actionable(&p, 0, now, window) ==
                      kdash_cmd_actionable(p.ts, 0, now, window),
                  "the typed helper and the generic one agree");
        }

        /* One acted stamp per VERB. A mode switch acted on at T must not
         * suppress a screenshot asked for at the same instant — which is the
         * whole reason these are separate keys (CD-22). */
        {
            kdash_panelmode_t m;
            kdash_panelshot_t s;
            memset(&m, 0, sizeof(m));
            memset(&s, 0, sizeof(s));
            m.ts = cmd;
            s.ts = cmd;
            double acted_mode = m.ts; /* the mode command has been acted on */
            double acted_shot = 0;    /* the screenshot has not */
            CHECK(!kdash_cmd_actionable(m.ts, acted_mode, now, window),
                  "the mode command is spent");
            CHECK(kdash_cmd_actionable(s.ts, acted_shot, now, window),
                  "and the screenshot at the same stamp is still pending");
        }
    }

    /* ---- claude:session (HASH field/value pairs) ---- */
    {
        kdash_claude_session_t s;
        const char *f[] = {"host",  "project", "cwd",    "status",
                           "ts",    "started_ts", "model", "title"};
        const char *v[] = {"kai",   "kdashdata", "/home/ken/src/tools/kdashdata",
                           "working", "1756600000", "1756599000", "Opus 5",
                           "the claude readers"};
        CHECK(kdash_parse_claude_session("kai", "abc123", f, v, 8, &s),
              "full session hash");
        CHECK(strcmp(s.host, "kai") == 0 && strcmp(s.sid, "abc123") == 0,
              "identity comes from the key, not the payload echo");
        CHECK(s.status == KDASH_CLAUDE_WORKING, "status");
        CHECK(s.ts == 1756600000.0, "ts");
        CHECK(s.started_ts == 1756599000.0, "started_ts");
        CHECK(strcmp(s.project, "kdashdata") == 0, "project");
        CHECK(strcmp(s.model, "Opus 5") == 0, "model");
        CHECK(strcmp(s.title, "the claude readers") == 0, "title");

        /* status + ts are the whole liveness contract. */
        const char *only_ts_f[] = {"ts"};
        const char *only_ts_v[] = {"1756600000"};
        CHECK(!kdash_parse_claude_session("kai", "abc", only_ts_f, only_ts_v, 1,
                                          &s),
              "a keepalive-only hash is not a session (resurrection guard)");
        const char *no_ts_f[] = {"status", "project"};
        const char *no_ts_v[] = {"working", "kdashdata"};
        CHECK(!kdash_parse_claude_session("kai", "abc", no_ts_f, no_ts_v, 2, &s),
              "no ts, no record");

        /* The status enum is closed: an unknown word distrusts the record. */
        const char *bad_f[] = {"status", "ts"};
        const char *bad_v[] = {"thinking", "1756600000"};
        CHECK(!kdash_parse_claude_session("kai", "abc", bad_f, bad_v, 2, &s),
              "unrecognised status rejects the whole record");
        CHECK(s.ts == 0 && s.host[0] == '\0',
              "a rejected record leaves *out zeroed, not half-filled");

        /* ts arrives as a STRING off the wire and is parsed strictly: not a
         * number, not a record. Zero and negatives are not stamps either. */
        for (int i = 0; i < 4; i++) {
            const char *bad_ts[] = {"", "later", "0", "-5"};
            const char *tf[] = {"status", "ts"};
            const char *tv[] = {"awaiting", bad_ts[i]};
            CHECK(!kdash_parse_claude_session("kai", "abc", tf, tv, 2, &s),
                  "ts \"%s\" must reject", bad_ts[i]);
        }
        {
            const char *tf[] = {"status", "ts"};
            const char *tv[] = {"blocked", "1756600000junk"};
            CHECK(!kdash_parse_claude_session("kai", "abc", tf, tv, 2, &s),
                  "trailing junk on ts must reject");
        }

        /* Minimal, and the optional fields stay empty rather than invented. */
        const char *min_f[] = {"status", "ts"};
        const char *min_v[] = {"blocked", "1756600000"};
        CHECK(kdash_parse_claude_session("kubs0", "s1", min_f, min_v, 2, &s),
              "status + ts is enough");
        CHECK(s.status == KDASH_CLAUDE_BLOCKED, "blocked");
        CHECK(s.project[0] == '\0',
              "an absent project stays empty — a \"?\" placeholder is the "
              "panel's business (CD-10)");
        CHECK(s.started_ts == 0, "absent started_ts is 0, i.e. unknown");

        /* Additive evolution: a field nobody has heard of is ignored. */
        const char *new_f[] = {"status", "ts", "invented_next_sprint"};
        const char *new_v[] = {"awaiting", "1756600000", "whatever"};
        CHECK(kdash_parse_claude_session("kai", "abc", new_f, new_v, 3, &s),
              "unknown fields must be ignored, never rejected");

        /* A key segment that fails the token contract has no record either. */
        CHECK(!kdash_parse_claude_session("kai:evil", "abc", min_f, min_v, 2, &s),
              "host revalidated at the parse boundary");
        CHECK(!kdash_parse_claude_session("kai", "", min_f, min_v, 2, &s),
              "empty sid");

        /* A NULL field or value skips that pair rather than crashing. */
        const char *hole_f[] = {NULL, "status", "ts"};
        const char *hole_v[] = {"x", "working", "1756600000"};
        CHECK(kdash_parse_claude_session("kai", "abc", hole_f, hole_v, 3, &s),
              "a NULL field pointer skips the pair");
    }

    /* ---- claude display ladder ---- */
    {
        const long long idle = KDASH_CLAUDE_IDLE_S, stale = KDASH_CLAUDE_STALE_S;

        CHECK(kdash_claude_display(KDASH_CLAUDE_WORKING, 0, idle, stale) ==
                  KDASH_CLAUDE_DISP_WORKING,
              "fresh working");
        CHECK(kdash_claude_display(KDASH_CLAUDE_WORKING, idle - 1, idle, stale) ==
                  KDASH_CLAUDE_DISP_WORKING,
              "one second before idle is still working");
        CHECK(kdash_claude_display(KDASH_CLAUDE_WORKING, idle, idle, stale) ==
                  KDASH_CLAUDE_DISP_IDLE,
              "idle begins at idle_s, inclusive");
        CHECK(kdash_claude_display(KDASH_CLAUDE_WORKING, stale, idle, stale) ==
                  KDASH_CLAUDE_DISP_STALE,
              "stale begins at stale_s, inclusive");

        /* Only `working` degrades to idle: a turn does not stop being yours
         * because you took twenty minutes over it. */
        CHECK(kdash_claude_display(KDASH_CLAUDE_BLOCKED, idle + 60, idle, stale) ==
                  KDASH_CLAUDE_DISP_BLOCKED,
              "blocked stays prominent through the idle band");
        CHECK(kdash_claude_display(KDASH_CLAUDE_AWAITING, idle + 60, idle,
                                   stale) == KDASH_CLAUDE_DISP_AWAITING,
              "awaiting stays prominent through the idle band");

        /* ...but age has the last say, because the hooks cannot report a
         * killed process. */
        CHECK(kdash_claude_display(KDASH_CLAUDE_BLOCKED, stale, idle, stale) ==
                  KDASH_CLAUDE_DISP_STALE,
              "stale outranks even blocked");
        CHECK(kdash_claude_display(KDASH_CLAUDE_AWAITING, stale + 9999, idle,
                                   stale) == KDASH_CLAUDE_DISP_STALE,
              "stale outranks awaiting");

        /* Thresholds are parameters, not policy baked into the derivation. */
        CHECK(kdash_claude_display(KDASH_CLAUDE_WORKING, 10, 5, 20) ==
                  KDASH_CLAUDE_DISP_IDLE,
              "a caller's own thresholds are honoured");

        CHECK(strcmp(kdash_claude_disp_str(KDASH_CLAUDE_DISP_BLOCKED),
                     "blocked") == 0,
              "disp label is the enum's own word, not a display string");
        CHECK(strcmp(kdash_claude_disp_str(KDASH_CLAUDE_DISP_STALE), "stale") == 0,
              "stale label");
        CHECK(strcmp(kdash_claude_status_str(KDASH_CLAUDE_AWAITING),
                     "awaiting") == 0,
              "status round-trips to the schema's own word");
        kdash_claude_status_t st;
        CHECK(kdash_claude_status_from_str("blocked", &st) &&
                  st == KDASH_CLAUDE_BLOCKED,
              "status parses from the schema's word");
        CHECK(!kdash_claude_status_from_str("Working", &st),
              "the enum is case-sensitive and closed");
    }

    /* ---- attention-first ordering ---- */
    {
        const long long now = 1756600000;
        kdash_claude_session_t a[5];
        memset(a, 0, sizeof(a));

        /* Deliberately out of order, with a tie to settle. */
        snprintf(a[0].host, sizeof(a[0].host), "kai");
        snprintf(a[0].sid, sizeof(a[0].sid), "old-working");
        a[0].status = KDASH_CLAUDE_WORKING;
        a[0].ts = (double)(now - KDASH_CLAUDE_STALE_S - 10); /* stale */

        snprintf(a[1].host, sizeof(a[1].host), "kubs0");
        snprintf(a[1].sid, sizeof(a[1].sid), "working");
        a[1].status = KDASH_CLAUDE_WORKING;
        a[1].ts = (double)(now - 5);

        snprintf(a[2].host, sizeof(a[2].host), "cleo");
        snprintf(a[2].sid, sizeof(a[2].sid), "blocked");
        a[2].status = KDASH_CLAUDE_BLOCKED;
        a[2].ts = (double)(now - 30);

        snprintf(a[3].host, sizeof(a[3].host), "kai");
        snprintf(a[3].sid, sizeof(a[3].sid), "awaiting-b");
        a[3].status = KDASH_CLAUDE_AWAITING;
        a[3].ts = (double)(now - 60);

        snprintf(a[4].host, sizeof(a[4].host), "kai");
        snprintf(a[4].sid, sizeof(a[4].sid), "awaiting-a");
        a[4].status = KDASH_CLAUDE_AWAITING;
        a[4].ts = (double)(now - 60); /* ties a[3] on rank AND ts */

        kdash_claude_sessions_refresh(a, 5, now, KDASH_CLAUDE_IDLE_S,
                                      KDASH_CLAUDE_STALE_S);

        CHECK(strcmp(a[0].sid, "blocked") == 0, "blocked first, got \"%s\"",
              a[0].sid);
        CHECK(a[1].disp == KDASH_CLAUDE_DISP_AWAITING &&
                  a[2].disp == KDASH_CLAUDE_DISP_AWAITING,
              "both awaiting rows next");
        /* Same rank, same ts: host then sid, so the order is stable across
         * renders rather than whatever the SCAN happened to return. */
        CHECK(strcmp(a[1].sid, "awaiting-a") == 0 &&
                  strcmp(a[2].sid, "awaiting-b") == 0,
              "a rank+ts tie breaks on host/sid, not on arrival order");
        CHECK(strcmp(a[3].sid, "working") == 0, "fresh working above stale");
        CHECK(a[4].disp == KDASH_CLAUDE_DISP_STALE, "stale last");

        /* Skew is not a huge age: a ts in the future reads as fresh. */
        kdash_claude_session_t one = {0};
        one.status = KDASH_CLAUDE_WORKING;
        one.ts = (double)(now + 3600);
        kdash_claude_sessions_refresh(&one, 1, now, KDASH_CLAUDE_IDLE_S,
                                      KDASH_CLAUDE_STALE_S);
        CHECK(one.disp == KDASH_CLAUDE_DISP_WORKING,
              "writer-clock skew counts as fresh, never as stale");

        kdash_claude_sessions_refresh(NULL, 3, now, 1, 2); /* must not crash */
        kdash_claude_sessions_refresh(a, 0, now, 1, 2);
    }

    /* ---- claude:limits ---- */
    {
        kdash_claude_limits_t l;
        const char *f[] = {"five_hour_pct",   "seven_day_pct",
                           "five_hour_resets_at", "seven_day_resets_at",
                           "updated_at",      "expected_refresh_s",
                           "host",            "source",
                           "scoped_model",    "scoped_pct",
                           "scoped_resets_at", "scoped_active",
                           "scoped_updated_at", "scoped_expected_refresh_s"};
        const char *v[] = {"41.5",       "77",         "1756610000", "1757000000",
                           "1756600000", "60",         "kai",        "statusline",
                           "Fable",      "12.5",       "1757100000", "1",
                           "1756599000", "300"};
        CHECK(kdash_parse_claude_limits(f, v, 14, &l), "full limits hash");
        CHECK(l.valid && l.five_hour_pct == 41.5 && l.seven_day_pct == 77.0,
              "headline percentages");
        CHECK(l.five_hour_resets_at == 1756610000.0, "five-hour reset");
        CHECK(l.updated_at == 1756600000.0 && l.expected_refresh_s == 60.0,
              "observation stamp and cadence");
        CHECK(l.scoped_valid && strcmp(l.scoped_model, "Fable") == 0 &&
                  l.scoped_pct == 12.5,
              "scoped set");
        CHECK(l.scoped_active, "scoped_active");
        CHECK(l.scoped_updated_at == 1756599000.0 &&
                  l.scoped_expected_refresh_s == 300.0,
              "the scoped set carries its OWN stamp and cadence");

        /* Both percentages are required, and clamped. */
        const char *min_f[] = {"five_hour_pct", "seven_day_pct"};
        const char *min_v[] = {"-3", "140"};
        CHECK(kdash_parse_claude_limits(min_f, min_v, 2, &l), "minimal limits");
        CHECK(l.five_hour_pct == 0.0 && l.seven_day_pct == 100.0,
              "percentages clamp to [0,100]");
        CHECK(!l.scoped_valid, "no scoped set");

        const char *half_f[] = {"five_hour_pct"};
        const char *half_v[] = {"41"};
        CHECK(!kdash_parse_claude_limits(half_f, half_v, 1, &l),
              "one percentage is not a snapshot");
        CHECK(!l.valid, "a rejected snapshot is zeroed");

        const char *nan_f[] = {"five_hour_pct", "seven_day_pct"};
        const char *nan_v[] = {"nan", "77"};
        CHECK(!kdash_parse_claude_limits(nan_f, nan_v, 2, &l),
              "a non-finite percentage rejects");
        const char *junk_v[] = {"41%", "77"};
        CHECK(!kdash_parse_claude_limits(nan_f, junk_v, 2, &l),
              "trailing junk on a percentage rejects");

        /* Half a scoped set renders as no scoped set at all. */
        const char *sc_f[] = {"five_hour_pct", "seven_day_pct", "scoped_pct"};
        const char *sc_v[] = {"41", "77", "12"};
        CHECK(kdash_parse_claude_limits(sc_f, sc_v, 3, &l), "pct without label");
        CHECK(!l.scoped_valid,
              "a scoped percentage with no label is no gauge, not an "
              "unlabelled one");
        const char *sl_f[] = {"five_hour_pct", "seven_day_pct", "scoped_model"};
        const char *sl_v[] = {"41", "77", "Fable"};
        CHECK(kdash_parse_claude_limits(sl_f, sl_v, 3, &l), "label without pct");
        CHECK(!l.scoped_valid, "and the reverse is no gauge either");

        /* No five-hour window (CD-29). */
        CHECK(kdash_parse_claude_limits(f, v, 14, &l) && !l.five_hour_none,
              "a hash that predates five_hour_none has a five-hour window");
        CHECK(l.five_hour_pct == 41.5, "and its percentage is a reading");

        const char *nw_f[] = {"five_hour_pct", "seven_day_pct",
                              "five_hour_resets_at", "seven_day_resets_at",
                              "five_hour_none", "updated_at",
                              "expected_refresh_s"};
        const char *nw_v[] = {"0", "10", "0", "1763553600", "1", "1756600000",
                              "300"};
        CHECK(kdash_parse_claude_limits(nw_f, nw_v, 7, &l),
              "no five-hour window is a valid record");
        CHECK(l.valid && l.five_hour_none, "and says so");
        CHECK(l.seven_day_pct == 10.0 && l.seven_day_resets_at == 1763553600.0,
              "the seven-day gauge is untouched");

        /* The stale leftover: an earlier writer's five-hour numbers are still
         * in the hash beside the flag. They must not render as a reading. */
        const char *lo_v[] = {"37", "10", "1756610000", "1763553600", "1",
                              "1756600000", "300"};
        CHECK(kdash_parse_claude_limits(nw_f, lo_v, 7, &l) && l.five_hour_none,
              "leftover five-hour fields beside the flag");
        CHECK(l.five_hour_pct == 0.0 && l.five_hour_resets_at == 0.0,
              "are zeroed, whatever the hash holds");

        /* 0 says the window is present; junk never says it is gone. */
        const char *zero_v[] = {"37", "10", "1756610000", "1763553600", "0",
                                "1756600000", "300"};
        CHECK(kdash_parse_claude_limits(nw_f, zero_v, 7, &l) &&
                  !l.five_hour_none && l.five_hour_pct == 37.0 &&
                  l.five_hour_resets_at == 1756610000.0,
              "five_hour_none 0 is a window, published as is");
        const char *bad_none[] = {"true", "2", "1.0", ""};
        for (int i = 0; i < 4; i++) {
            const char *bv[] = {"37", "10", "1756610000", "1763553600",
                                bad_none[i], "1756600000", "300"};
            CHECK(kdash_parse_claude_limits(nw_f, bv, 7, &l) &&
                      !l.five_hour_none && l.five_hour_pct == 37.0,
                  "only exactly 1 says there is no window");
        }

        /* Unknown is not "no window": a rejected record is zeroed, and its
         * flag is false. */
        const char *rj_f[] = {"five_hour_none", "seven_day_pct"};
        const char *rj_v[] = {"1", "10"};
        CHECK(!kdash_parse_claude_limits(rj_f, rj_v, 2, &l) && !l.valid &&
                  !l.five_hour_none,
              "a record without five_hour_pct still rejects, flag or not");
    }

    /* ---- claude:limits staleness, per gauge ---- */
    {
        const long long grace = KDASH_CLAUDE_LIMITS_GRACE_S;
        const long long legacy = KDASH_CLAUDE_LIMITS_STALE_S;
        const long long now = 1756600000;

        kdash_claude_limits_t l;
        memset(&l, 0, sizeof(l));
        l.valid = true;
        l.expected_refresh_s = 60;
        l.updated_at = (double)(now - 100); /* < 60 + 60 */
        CHECK(!kdash_claude_limits_stale(&l, now, grace, legacy),
              "within cadence + grace");
        l.updated_at = (double)(now - 200); /* > 60 + 60 */
        CHECK(kdash_claude_limits_stale(&l, now, grace, legacy),
              "past cadence + grace");

        /* "No five-hour window" is a headline observation like any other: it
         * rides updated_at and goes stale with it (CD-29). */
        l.five_hour_none = true;
        CHECK(kdash_claude_limits_stale(&l, now, grace, legacy),
              "an old no-window statement is stale, not an all-clear");
        l.updated_at = (double)(now - 100);
        CHECK(!kdash_claude_limits_stale(&l, now, grace, legacy),
              "and a fresh one is fresh");
        l.five_hour_none = false;
        l.updated_at = (double)(now - 200);

        /* A pre-cadence writer published no cadence; it gets the legacy window
         * rather than being treated as instantly stale. */
        l.expected_refresh_s = 0;
        l.updated_at = (double)(now - 200);
        CHECK(!kdash_claude_limits_stale(&l, now, grace, legacy),
              "no cadence falls back to the legacy fixed window");
        l.updated_at = (double)(now - legacy - 1);
        CHECK(kdash_claude_limits_stale(&l, now, grace, legacy),
              "and past that window it is stale");

        /* An invalid snapshot is not stale, it is nothing. */
        memset(&l, 0, sizeof(l));
        CHECK(!kdash_claude_limits_stale(&l, now, grace, legacy),
              "an invalid snapshot has no staleness to report");
        CHECK(!kdash_claude_limits_scoped_stale(&l, now, grace, legacy),
              "nor does an absent scoped set");

        /* The scoped set is judged against ITS OWN stamp, never the
         * headline's — a statusline write cannot touch these numbers, so
         * letting them borrow its freshness would freeze the gauge invisibly. */
        memset(&l, 0, sizeof(l));
        l.valid = true;
        l.scoped_valid = true;
        l.expected_refresh_s = 60;
        l.updated_at = (double)now; /* headline just refreshed */
        l.scoped_expected_refresh_s = 300;
        l.scoped_updated_at = (double)(now - 4000);
        CHECK(!kdash_claude_limits_stale(&l, now, grace, legacy),
              "headline is fresh");
        CHECK(kdash_claude_limits_scoped_stale(&l, now, grace, legacy),
              "and the scoped gauge is stale anyway — its own stamp says so");

        /* Stampless means always stale, never "as fresh as the headline". The
         * second case is the one that actually pins the rule: at a `now` small
         * enough that "age since stamp 0" falls INSIDE the window, the
         * arithmetic alone would call a stampless gauge fresh. */
        l.scoped_updated_at = 0;
        CHECK(kdash_claude_limits_scoped_stale(&l, now, grace, legacy),
              "a stampless scoped set never borrows headline freshness");
        {
            kdash_claude_limits_t z;
            memset(&z, 0, sizeof(z));
            z.valid = true;
            z.scoped_valid = true;
            z.scoped_updated_at = 0; /* stampless */
            CHECK(kdash_claude_limits_scoped_stale(&z, 500, grace, legacy),
                  "stampless is stale by rule, not by epoch arithmetic");
        }

        /* Skew is never stale. */
        l.scoped_updated_at = (double)(now + 500);
        CHECK(!kdash_claude_limits_scoped_stale(&l, now, grace, legacy),
              "a stamp in the future is not stale");
    }

    /* ---- claude:recent ---- */
    {
        kdash_claude_recent_t r;
        CHECK(PARSE(kdash_parse_claude_recent,
                    "{\"host\":\"kai\",\"project\":\"kdashdata\","
                    "\"title\":\"the claude readers\",\"ended_ts\":1756600000,"
                    "\"dur_s\":900,\"ts\":1756600000.5}",
                    &r),
              "full recent record");
        CHECK(strcmp(r.host, "kai") == 0, "host");
        CHECK(strcmp(r.project, "kdashdata") == 0, "project");
        CHECK(strcmp(r.title, "the claude readers") == 0, "title");
        CHECK(r.ended_ts == 1756600000.0 && r.dur_s == 900.0, "stamps");

        CHECK(PARSE(kdash_parse_claude_recent,
                    "{\"host\":\"kai\",\"project\":\"kdashdata\"}", &r),
              "title, ended_ts and dur_s are all optional");
        CHECK(r.title[0] == '\0', "a session Claude never named has no title");
        CHECK(r.dur_s == 0, "unknown duration is 0");

        CHECK(!PARSE(kdash_parse_claude_recent, "{\"project\":\"kdashdata\"}", &r),
              "no host, no record");
        CHECK(!PARSE(kdash_parse_claude_recent,
                     "{\"host\":\"kai\",\"project\":\"\"}", &r),
              "an empty project skips the record");
        CHECK(!PARSE(kdash_parse_claude_recent,
                     "{\"host\":\"kai/evil\",\"project\":\"p\"}", &r),
              "a host failing the token contract skips the record");
        CHECK(r.project[0] == '\0', "a rejected record is zeroed");
        CHECK(!PARSE(kdash_parse_claude_recent, "[1,2,3]", &r),
              "not an object");

        CHECK(PARSE(kdash_parse_claude_recent,
                    "{\"host\":\"kai\",\"project\":\"p\",\"invented\":true}", &r),
              "unknown fields must be ignored, never rejected");
    }

    /* Apartment-temperature bands (CD-16 applied to apttemps). These must
     * reproduce kpidash's `< 65 / <= 75 / < 80 / else` EXACTLY — the whole
     * point of moving the classification here is that the panels keep
     * agreeing, so every boundary is asserted on both sides. */
    {
#define BAND(t, s)                                                             \
    kdash_apttemps_band((t), (s), KDASH_APTTEMPS_COLD_F, KDASH_APTTEMPS_OK_F,  \
                        KDASH_APTTEMPS_HOT_F)

        CHECK(BAND(40.0, false) == KDASH_TEMP_COLD, "well below is cold");
        CHECK(BAND(64.9, false) == KDASH_TEMP_COLD, "just below 65 is cold");
        CHECK(BAND(65.0, false) == KDASH_TEMP_OK, "65.0 is ok, not cold");
        CHECK(BAND(70.0, false) == KDASH_TEMP_OK, "mid-band is ok");
        CHECK(BAND(75.0, false) == KDASH_TEMP_OK, "75.0 is ok — inclusive");
        CHECK(BAND(75.1, false) == KDASH_TEMP_WARM, "75.1 is warm");
        CHECK(BAND(79.9, false) == KDASH_TEMP_WARM, "79.9 is warm");
        CHECK(BAND(80.0, false) == KDASH_TEMP_HOT, "80.0 is hot, not warm");
        CHECK(BAND(104.0, false) == KDASH_TEMP_HOT, "well above is hot");

        /* Age beats the number, at every band. */
        CHECK(BAND(70.0, true) == KDASH_TEMP_STALE, "a stale comfortable room");
        CHECK(BAND(104.0, true) == KDASH_TEMP_STALE, "a stale hot room");

        /* Thresholds are parameters, not policy. */
        CHECK(kdash_apttemps_band(70.0, false, 72.0, 80.0, 90.0) ==
                  KDASH_TEMP_COLD,
              "a caller's own colder band moves the verdict");

        /* Out-of-order boundaries clamp rather than produce an unreachable
         * band — the guarantee kdash_ladder() gives for stale_s < idle_s.
         *
         * This case is the one that can tell the clamp apart from its
         * absence, and finding it took a negative test: cold_f == temp_f with
         * ok_f BELOW cold_f. Clamped, ok_f becomes 80 and 80.0 lands `ok`.
         * Unclamped, `80 <= 60` fails and the reading falls through to warm —
         * a room at exactly the comfortable boundary rendered as warm. */
        CHECK(kdash_apttemps_band(80.0, false, 80.0, 60.0, 100.0) ==
                  KDASH_TEMP_OK,
              "ok_f below cold_f clamps up, so cold_f itself is still ok");
        CHECK(kdash_apttemps_band(79.0, false, 80.0, 60.0, 100.0) ==
                  KDASH_TEMP_COLD,
              "below a clamped cold_f is still cold");
        CHECK(kdash_apttemps_band(90.0, false, 80.0, 60.0, 100.0) ==
                  KDASH_TEMP_WARM,
              "above the clamped ok_f, the caller's hot_f still governs");

        CHECK(strcmp(kdash_temp_band_label(KDASH_TEMP_COLD), "cold") == 0, "cold");
        CHECK(strcmp(kdash_temp_band_label(KDASH_TEMP_OK), "ok") == 0, "ok");
        CHECK(strcmp(kdash_temp_band_label(KDASH_TEMP_WARM), "warm") == 0, "warm");
        CHECK(strcmp(kdash_temp_band_label(KDASH_TEMP_HOT), "hot") == 0, "hot");
        CHECK(strcmp(kdash_temp_band_label(KDASH_TEMP_STALE), "stale") == 0,
              "stale");
        CHECK(kdash_temp_band_label((kdash_temp_band_t)99) != NULL,
              "never NULL, even off the enum");
#undef BAND
    }

    /* ---- kdash:korg:pulse (CD-28) ---- */
    {
        kdash_korg_pulse_t k;

        /* A full record: the schema's first example, trimmed to four
         * projects and three days. */
        CHECK(PARSE(kdash_parse_korg_pulse,
                    "{\"ts\":1794617100.4,\"expected_refresh_s\":60,"
                    "\"open\":25,\"projects\":16,\"awaiting\":1,\"active\":0,"
                    "\"queued\":1,\"other_open\":12,"
                    "\"by_project\":[{\"name\":\"kligd\",\"open\":5},"
                    "{\"name\":\"k-homelab\",\"open\":4},"
                    "{\"name\":\"kdeskdash\",\"open\":2},"
                    "{\"name\":\"kloom\",\"open\":2}],"
                    "\"history\":[{\"date\":\"2026-11-11\",\"open\":21},"
                    "{\"date\":\"2026-11-12\",\"open\":22},"
                    "{\"date\":\"2026-11-13\",\"open\":25}],"
                    "\"unknown_future_field\":{\"x\":1}}",
                    &k),
              "full korg pulse, with an unknown field ignored");
        CHECK(k.ts > 1794617100.0 && k.expected_refresh_s == 60.0, "head");
        CHECK(k.open == 25 && k.projects == 16 && k.awaiting == 1 &&
                  k.active == 0 && k.queued == 1 && k.other_open == 12,
              "counts");
        CHECK(k.by_project_count == 4 && !k.by_project_truncated &&
                  k.by_project_skipped == 0,
              "four projects, nothing cut or skipped");
        CHECK(strcmp(k.by_project[0].name, "kligd") == 0 &&
                  k.by_project[0].open == 5,
              "the top project is the first entry, in the writer's order");
        CHECK(k.history_count == 3 &&
                  strcmp(k.history[2].date, "2026-11-13") == 0 &&
                  k.history[2].open == k.open,
              "history oldest first; today's point is the headline");

        /* A minimal record: every count 0, both arrays empty — korg with
         * nothing open, or a publisher whose flow window is not there yet. */
        CHECK(PARSE(kdash_parse_korg_pulse,
                    "{\"ts\":1,\"expected_refresh_s\":1,\"open\":0,"
                    "\"projects\":0,\"awaiting\":0,\"active\":0,\"queued\":0,"
                    "\"other_open\":0,\"by_project\":[],\"history\":[]}",
                    &k),
              "minimal korg pulse");
        CHECK(k.by_project_count == 0 && k.history_count == 0 &&
                  !k.history_truncated,
              "empty arrays are a statement, not an error");

        /* Empty history beside a real backlog: the transition log does not
         * reach back yet. The card draws the number without the line. */
        CHECK(PARSE(kdash_parse_korg_pulse,
                    "{\"ts\":1794617100,\"expected_refresh_s\":60,\"open\":3,"
                    "\"projects\":1,\"awaiting\":0,\"active\":0,\"queued\":0,"
                    "\"other_open\":0,\"by_project\":[{\"name\":\"korg\",\"open\":3}],"
                    "\"history\":[]}",
                    &k),
              "empty history with a non-empty backlog");
        CHECK(k.open == 3 && k.history_count == 0 && k.by_project_count == 1,
              "the record stands without its series");

        /* Over-long arrays: 26 projects and 32 days. The FIRST 24 projects
         * (the largest) and the NEWEST 30 days (today included) are kept. */
        {
            char json[4096];
            int n = snprintf(json, sizeof(json),
                             "{\"ts\":1794617100,\"expected_refresh_s\":60,"
                             "\"open\":100,\"projects\":26,\"awaiting\":0,"
                             "\"active\":0,\"queued\":0,\"other_open\":0,"
                             "\"by_project\":[");
            for (int i = 0; i < 26; i++)
                n += snprintf(json + n, sizeof(json) - n,
                              "%s{\"name\":\"p%02d\",\"open\":%d}", i ? "," : "",
                              i + 1, 30 - i);
            n += snprintf(json + n, sizeof(json) - n, "],\"history\":[");
            for (int d = 1; d <= 32; d++)
                n += snprintf(json + n, sizeof(json) - n,
                              "%s{\"date\":\"2026-10-%02d\",\"open\":%d}",
                              d > 1 ? "," : "", d, d);
            snprintf(json + n, sizeof(json) - n, "]}");

            CHECK(PARSE(kdash_parse_korg_pulse, json, &k),
                  "an over-long record still parses");
            CHECK(k.by_project_count == KDASH_KORG_PROJECTS_MAX &&
                      k.by_project_truncated,
                  "projects capped at 24 and flagged");
            CHECK(strcmp(k.by_project[0].name, "p01") == 0 &&
                      strcmp(k.by_project[23].name, "p24") == 0,
                  "the first 24 — the largest — are the ones kept");
            CHECK(k.history_count == KDASH_KORG_HISTORY_MAX && k.history_truncated,
                  "history capped at 30 and flagged");
            CHECK(strcmp(k.history[0].date, "2026-10-03") == 0 &&
                      strcmp(k.history[29].date, "2026-10-32") == 0,
                  "the NEWEST 30 are kept, so today survives the cut");
        }

        /* A malformed entry is skipped and counted; the record stands. */
        CHECK(PARSE(kdash_parse_korg_pulse,
                    "{\"ts\":1,\"expected_refresh_s\":60,\"open\":3,"
                    "\"projects\":2,\"awaiting\":0,\"active\":0,\"queued\":0,"
                    "\"other_open\":0,"
                    "\"by_project\":[{\"name\":\"korg\",\"open\":3},"
                    "{\"name\":\"klams\",\"open\":0},{\"open\":1},\"x\"],"
                    "\"history\":[{\"date\":\"11/13/2026\",\"open\":3},"
                    "{\"date\":\"2026-11-13\",\"open\":3.5},"
                    "{\"date\":\"2026-11-13\",\"open\":3}]}",
                    &k),
              "bad entries do not cost the record");
        CHECK(k.by_project_count == 1 && k.by_project_skipped == 3,
              "a zero-open, a nameless and a non-object entry are skipped");
        CHECK(k.history_count == 1 && k.history_skipped == 2,
              "a locale date and a fractional count are skipped");

        /* Required fields reject the whole record, and leave it zeroed. */
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"open\":25,"
                     "\"projects\":1,\"awaiting\":1,\"active\":0,\"queued\":1,"
                     "\"by_project\":[],\"history\":[]}",
                     &k),
              "other_open absent rejects (the overflow is never silent)");
        CHECK(k.open == 0 && k.ts == 0, "and the struct is zeroed");
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1,\"open\":0,\"projects\":0,\"awaiting\":0,"
                     "\"active\":0,\"queued\":0,\"other_open\":0,"
                     "\"by_project\":[],\"history\":[]}",
                     &k),
              "expected_refresh_s absent rejects: no cadence, no staleness");
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1794617100400,\"expected_refresh_s\":60,\"open\":0,"
                     "\"projects\":0,\"awaiting\":0,\"active\":0,\"queued\":0,"
                     "\"other_open\":0,\"by_project\":[],\"history\":[]}",
                     &k),
              "a millisecond ts rejects");
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"open\":-1,"
                     "\"projects\":0,\"awaiting\":0,\"active\":0,\"queued\":0,"
                     "\"other_open\":0,\"by_project\":[],\"history\":[]}",
                     &k),
              "a negative count rejects");
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"open\":0,"
                     "\"projects\":0,\"awaiting\":0,\"active\":0,\"queued\":0,"
                     "\"other_open\":0,\"by_project\":{},\"history\":[]}",
                     &k),
              "a by_project that is not an array rejects");
        CHECK(!PARSE(kdash_parse_korg_pulse,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"open\":0,"
                     "\"projects\":0,\"awaiting\":0,\"active\":0,\"queued\":0,"
                     "\"other_open\":0,\"by_project\":[]}",
                     &k),
              "history absent rejects; empty is the way to say none");
    }

    /* ---- kdash:klams:status (CD-28) ---- */
    {
        kdash_klams_status_t s;

        CHECK(PARSE(kdash_parse_klams_status,
                    "{\"ts\":1794617160.2,\"expected_refresh_s\":60,"
                    "\"health\":\"ok\",\"version\":\"0.1.58\",\"uptime_s\":57600,"
                    "\"knowledge\":228934,\"facts\":40,\"events\":39,"
                    "\"scanners\":[{\"name\":\"kai\",\"last_wrote_at\":1794616020,"
                    "\"last_ran_at\":1794616020,\"expected_interval_s\":3600},"
                    "{\"name\":\"kubs0\",\"last_wrote_at\":1794358800,"
                    "\"last_ran_at\":1794615480,\"expected_interval_s\":3600}],"
                    "\"subsystems\":[{\"name\":\"postgres\",\"ok\":true},"
                    "{\"name\":\"rerank\",\"ok\":false}],"
                    "\"queue_depth\":0,\"queue_capacity\":1024,"
                    "\"backup_at\":1794573960,"
                    "\"new_per_day\":[{\"date\":\"2026-11-12\",\"added\":140},"
                    "{\"date\":\"2026-11-13\",\"added\":95}]}",
                    &s),
              "full klams status");
        CHECK(s.health == KDASH_KLAMS_OK && strcmp(s.version, "0.1.58") == 0 &&
                  s.uptime_s == 57600,
              "health, version, uptime");
        CHECK(s.knowledge == 228934 && s.facts == 40 && s.events == 39,
              "corpus counts");
        CHECK(s.scanners_count == 2 &&
                  strcmp(s.scanners[1].name, "kubs0") == 0 &&
                  s.scanners[1].last_wrote_at == 1794358800.0 &&
                  s.scanners[1].last_ran_at == 1794615480.0 &&
                  s.scanners[1].expected_interval_s == 3600.0,
              "wrote and ran are kept apart");
        CHECK(s.subsystems_count == 2 && s.subsystems[0].ok &&
                  !s.subsystems[1].ok,
              "subsystems");
        CHECK(s.queue_depth == 0 && s.queue_capacity == 1024 &&
                  s.backup_at == 1794573960.0,
              "queue and backup");
        CHECK(s.new_per_day_count == 2 && s.new_per_day[1].added == 95,
              "new_per_day");

        /* The `down` record: every klams-sourced value null, arrays empty.
         * This is the minimal record, and it must parse — a fresh `down` is
         * the publisher saying klams is down. */
        CHECK(PARSE(kdash_parse_klams_status,
                    "{\"ts\":1794617160,\"expected_refresh_s\":60,"
                    "\"health\":\"down\",\"version\":null,\"uptime_s\":null,"
                    "\"knowledge\":null,\"facts\":null,\"events\":null,"
                    "\"scanners\":[],\"subsystems\":[],\"queue_depth\":null,"
                    "\"queue_capacity\":null,\"backup_at\":null,"
                    "\"new_per_day\":[]}",
                    &s),
              "a down record parses");
        CHECK(s.health == KDASH_KLAMS_DOWN && s.version[0] == '\0', "down, no version");
        CHECK(s.knowledge == KDASH_COUNT_NULL && s.uptime_s == KDASH_COUNT_NULL &&
                  s.queue_depth == KDASH_COUNT_NULL &&
                  s.queue_capacity == KDASH_COUNT_NULL,
              "null counts read as KDASH_COUNT_NULL, never as 0");
        CHECK(s.backup_at == 0, "a null stamp reads as 0");

        /* last_ran_at: null is a statement, absent is not. */
        CHECK(PARSE(kdash_parse_klams_status,
                    "{\"ts\":1,\"expected_refresh_s\":60,\"health\":\"degraded\","
                    "\"version\":\"0.1.58\",\"uptime_s\":1,\"knowledge\":1,"
                    "\"facts\":1,\"events\":1,"
                    "\"scanners\":[{\"name\":\"kai\",\"last_wrote_at\":null,"
                    "\"last_ran_at\":null,\"expected_interval_s\":3600},"
                    "{\"name\":\"kubs0\",\"last_wrote_at\":1794616020,"
                    "\"expected_interval_s\":3600}],"
                    "\"subsystems\":[{\"name\":\"embed\"}],\"queue_depth\":0,"
                    "\"queue_capacity\":1024,\"backup_at\":null,"
                    "\"new_per_day\":[]}",
                    &s),
              "degraded record with a partly-bad scanner list");
        CHECK(s.health == KDASH_KLAMS_DEGRADED, "degraded");
        CHECK(s.scanners_count == 1 && s.scanners_skipped == 1 &&
                  s.scanners[0].last_wrote_at == 0 &&
                  s.scanners[0].last_ran_at == 0,
              "the null scanner is kept; the one missing last_ran_at is skipped");
        CHECK(s.subsystems_count == 0 && s.subsystems_skipped == 1,
              "a subsystem without `ok` is skipped");

        /* Over-long new_per_day keeps the NEWEST 14. */
        {
            char json[2048];
            int n = snprintf(json, sizeof(json),
                             "{\"ts\":1,\"expected_refresh_s\":60,\"health\":\"ok\","
                             "\"version\":\"v\",\"uptime_s\":1,\"knowledge\":1,"
                             "\"facts\":1,\"events\":1,\"scanners\":[],"
                             "\"subsystems\":[],\"queue_depth\":0,"
                             "\"queue_capacity\":1,\"backup_at\":null,"
                             "\"new_per_day\":[");
            for (int d = 1; d <= 16; d++)
                n += snprintf(json + n, sizeof(json) - n,
                              "%s{\"date\":\"2026-11-%02d\",\"added\":%d}",
                              d > 1 ? "," : "", d, d);
            snprintf(json + n, sizeof(json) - n, "]}");
            CHECK(PARSE(kdash_parse_klams_status, json, &s), "16 days parse");
            CHECK(s.new_per_day_count == KDASH_KLAMS_DAYS_MAX &&
                      s.new_per_day_truncated &&
                      strcmp(s.new_per_day[0].date, "2026-11-03") == 0 &&
                      s.new_per_day[13].added == 16,
                  "the newest 14 days are kept and the cut is flagged");
        }

        CHECK(!PARSE(kdash_parse_klams_status,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"health\":\"warn\","
                     "\"version\":null,\"uptime_s\":null,\"knowledge\":null,"
                     "\"facts\":null,\"events\":null,\"scanners\":[],"
                     "\"subsystems\":[],\"queue_depth\":null,"
                     "\"queue_capacity\":null,\"backup_at\":null,"
                     "\"new_per_day\":[]}",
                     &s),
              "health outside the closed set rejects");
        CHECK(!PARSE(kdash_parse_klams_status,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"health\":\"down\","
                     "\"version\":null,\"uptime_s\":null,"
                     "\"facts\":null,\"events\":null,\"scanners\":[],"
                     "\"subsystems\":[],\"queue_depth\":null,"
                     "\"queue_capacity\":null,\"backup_at\":null,"
                     "\"new_per_day\":[]}",
                     &s),
              "knowledge ABSENT rejects even on a down record (CD-27)");
        CHECK(!PARSE(kdash_parse_klams_status,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"health\":\"ok\","
                     "\"version\":\"v\",\"uptime_s\":1,\"knowledge\":1,"
                     "\"facts\":1,\"events\":1,\"scanners\":[],"
                     "\"subsystems\":[],\"queue_depth\":0,"
                     "\"queue_capacity\":1,\"backup_at\":1794573960000,"
                     "\"new_per_day\":[]}",
                     &s),
              "a millisecond backup_at rejects");
        CHECK(s.health == KDASH_KLAMS_OK && s.knowledge == 0,
              "a rejected record is zeroed, not left as KDASH_COUNT_NULL");

        kdash_klams_health_t hh;
        CHECK(kdash_klams_health_from_str("degraded", &hh) &&
                  hh == KDASH_KLAMS_DEGRADED &&
                  strcmp(kdash_klams_health_str(hh), "degraded") == 0,
              "health round-trips through its own word");
        CHECK(!kdash_klams_health_from_str("OK", &hh), "the set is lowercase");
    }

    /* Buffers are counted, not NUL-terminated: SCAN and GET both hand back
     * lengths, and an embedded NUL must not truncate the parse. */
    {
        kdash_health_t h;
        const char json[] = "{\"hostname\":\"kai\",\"last_seen_ts\":1}TRAILING";
        CHECK(kdash_parse_health(json, 36, &h), "length-bounded parse");
        CHECK(strcmp(h.hostname, "kai") == 0, "parsed under a length bound");
    }

    /* ---- kdash:calendar (CD-30) ---- */
    {
        kdash_calendar_t cal;
#define CAL_HEAD                                                               \
    "{\"ts\":1794643200.5,\"expected_refresh_s\":3600,"                        \
    "\"from\":\"2026-11-01\",\"to\":\"2027-02-12\","

        /* This month as the program's acceptance names it: the 11th, a run of
         * three days off, and the 27th carrying TWO tags in one entry. */
        CHECK(PARSE(kdash_parse_calendar,
                    CAL_HEAD "\"days\":["
                    "{\"date\":\"2026-11-11\",\"off\":false,"
                    "\"holiday\":\"Veterans Day\",\"short\":\"Veterans\"},"
                    "{\"date\":\"2026-11-25\",\"off\":true},"
                    "{\"date\":\"2026-11-26\",\"off\":true,"
                    "\"holiday\":\"Thanksgiving\",\"short\":\"Thanksgiving\"},"
                    "{\"date\":\"2026-11-27\",\"off\":true,"
                    "\"special\":\"Ken's birthday\",\"short\":\"Bday\"}],"
                    "\"v\":1,\"unknown_future_field\":{\"x\":1}}",
                    &cal),
              "full calendar, with `v` and an unknown field ignored");
        CHECK(cal.ts > 1794643200.0 && cal.expected_refresh_s == 3600.0, "head");
        CHECK(strcmp(cal.from, "2026-11-01") == 0 &&
                  strcmp(cal.to, "2027-02-12") == 0,
              "window");
        CHECK(cal.days_count == 4 && cal.days_skipped == 0 &&
                  !cal.days_truncated,
              "four days, nothing skipped or cut");
        CHECK(!cal.days[0].off &&
                  strcmp(cal.days[0].holiday, "Veterans Day") == 0 &&
                  cal.days[0].special[0] == '\0' &&
                  strcmp(cal.days[0].short_label, "Veterans") == 0,
              "a worked holiday: off false, named, no special");
        CHECK(cal.days[1].off && cal.days[1].holiday[0] == '\0' &&
                  cal.days[1].special[0] == '\0' &&
                  cal.days[1].short_label[0] == '\0',
              "a bare day off: absent strings read as empty");

        const kdash_calendar_day_t *d = kdash_calendar_day(&cal, "2026-11-27");
        CHECK(d && d->off && strcmp(d->special, "Ken's birthday") == 0 &&
                  strcmp(d->short_label, "Bday") == 0,
              "the 27th is a day off AND a birthday, in one entry");
        CHECK(kdash_calendar_day(&cal, "2026-11-12") == NULL,
              "an untagged date inside the window has no entry");
        CHECK(kdash_calendar_covers(&cal, "2026-11-14"), "today is covered");
        CHECK(kdash_calendar_covers(&cal, "2026-11-01") &&
                  kdash_calendar_covers(&cal, "2027-02-12"),
              "the window is inclusive at both ends");
        CHECK(!kdash_calendar_covers(&cal, "2026-10-31") &&
                  !kdash_calendar_covers(&cal, "2027-02-13"),
              "outside the window is not covered: the stale condition");
        CHECK(kdash_calendar_day(&cal, "2027-03-01") == NULL,
              "and outside it there is no entry to find");

        /* All three tags at once: an observed holiday Ken takes off, which is
         * also a special day, with one short label for the whole entry. */
        CHECK(PARSE(kdash_parse_calendar,
                    "{\"ts\":1783015200,\"expected_refresh_s\":3600,"
                    "\"from\":\"2026-07-01\",\"to\":\"2026-09-30\",\"days\":["
                    "{\"date\":\"2026-07-03\",\"off\":true,"
                    "\"holiday\":\"Independence Day (observed)\","
                    "\"special\":\"Lake trip\",\"short\":\"July 4th\"},"
                    "{\"date\":\"2026-07-04\",\"off\":false,"
                    "\"holiday\":\"Independence Day\"}]}",
                    &cal),
              "an entry with all three tags");
        d = kdash_calendar_day(&cal, "2026-07-03");
        CHECK(d && d->off &&
                  strcmp(d->holiday, "Independence Day (observed)") == 0 &&
                  strcmp(d->special, "Lake trip") == 0 &&
                  strcmp(d->short_label, "July 4th") == 0,
              "off, holiday, special and short all carried");

        /* Nothing tagged: a statement, and every covered date is untagged. */
        CHECK(PARSE(kdash_parse_calendar, CAL_HEAD "\"days\":[]}", &cal),
              "empty days");
        CHECK(cal.days_count == 0 && cal.days_skipped == 0 &&
                  kdash_calendar_covers(&cal, "2026-11-14") &&
                  kdash_calendar_day(&cal, "2026-11-14") == NULL,
              "an empty list still covers its window");

        /* Bad entries are skipped and counted; the record survives them. */
        CHECK(PARSE(kdash_parse_calendar,
                    CAL_HEAD "\"days\":["
                    "{\"date\":\"2026-11-02\",\"off\":false},"
                    "{\"date\":\"2026-10-31\",\"off\":true},"
                    "{\"date\":\"2027-02-13\",\"off\":true},"
                    "{\"date\":\"2026-11-03\",\"off\":true,\"holiday\":null},"
                    "{\"date\":\"2026-11-04\",\"off\":true,\"special\":\"\"},"
                    "{\"date\":\"2026-11-05\",\"off\":true,"
                    "\"short\":\"Thirteen chrs\"},"
                    "{\"date\":\"2026-11-06\",\"off\":true,"
                    "\"holiday\":\"abcdefghijklmnopqrstuvwxyz012345\"},"
                    "{\"date\":\"2026-11-07\",\"off\":\"yes\"},"
                    "{\"date\":\"2026-11-08\",\"holiday\":\"No off\"},"
                    "{\"date\":\"11/09/2026\",\"off\":true},"
                    "\"2026-11-10\","
                    "{\"date\":\"2026-11-26\",\"off\":true,"
                    "\"holiday\":\"Thanksgiving\"}]}",
                    &cal),
              "a record with eleven bad entries and one good one");
        CHECK(cal.days_count == 1 && cal.days_skipped == 11 &&
                  strcmp(cal.days[0].date, "2026-11-26") == 0,
              "only the good entry kept, %d skipped", cal.days_skipped);
        CHECK(kdash_calendar_day(&cal, "2026-11-02") == NULL,
              "an untagged entry is not a tagged day");

        /* Names are bounded in CHARACTERS, as the schema counts: 31 accented
         * characters are 62 bytes and fit; 32 do not. Likewise `short`. */
        CHECK(PARSE(kdash_parse_calendar,
                    CAL_HEAD "\"days\":["
                    "{\"date\":\"2026-11-02\",\"off\":false,\"holiday\":"
                    "\"ééééééééééééééééééééééééééééééé\",\"short\":\"Día de Ánimo\"},"
                    "{\"date\":\"2026-11-03\",\"off\":false,\"special\":"
                    "\"éééééééééééééééééééééééééééééééé\"}]}",
                    &cal),
              "two UTF-8 names");
        CHECK(cal.days_count == 1 && cal.days_skipped == 1 &&
                  strlen(cal.days[0].holiday) == 62 &&
                  strcmp(cal.days[0].short_label, "Día de Ánimo") == 0,
              "31 characters and a 12-character short kept whole, 32 skipped");

        /* Over the cap: the EARLIEST 64 kept, because they hold this month. */
        {
            static char big[16384];
            int n = snprintf(big, sizeof(big),
                             "{\"ts\":1794643200,\"expected_refresh_s\":3600,"
                             "\"from\":\"2026-01-01\",\"to\":\"2026-12-31\","
                             "\"days\":[");
            for (int i = 0; i < 70; i++)
                n += snprintf(big + n, sizeof(big) - n,
                              "%s{\"date\":\"2026-%02d-%02d\",\"off\":true}",
                              i ? "," : "", 1 + i / 28, 1 + i % 28);
            n += snprintf(big + n, sizeof(big) - n, "]}");
            CHECK(PARSE(kdash_parse_calendar, big, &cal), "70 entries parse");
            CHECK(cal.days_count == KDASH_CALENDAR_DAYS_MAX &&
                      cal.days_truncated && cal.days_skipped == 0,
                  "64 kept and flagged");
            CHECK(strcmp(cal.days[0].date, "2026-01-01") == 0 &&
                      strcmp(cal.days[63].date, "2026-03-08") == 0,
                  "the earliest 64, last kept \"%s\"", cal.days[63].date);
        }

        /* Record-level rejections: without a window, no absence means anything. */
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1,\"expected_refresh_s\":3600,"
                     "\"from\":\"2026-11-01\",\"days\":[]}",
                     &cal),
              "`to` absent rejects");
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1,\"expected_refresh_s\":3600,"
                     "\"from\":\"2026-11-01\",\"to\":\"2026-10-31\",\"days\":[]}",
                     &cal),
              "an inverted window rejects");
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1,\"expected_refresh_s\":3600,"
                     "\"from\":\"2026-11-1\",\"to\":\"2027-02-12\",\"days\":[]}",
                     &cal),
              "a malformed `from` rejects");
        CHECK(!PARSE(kdash_parse_calendar, CAL_HEAD "\"days\":{}}", &cal),
              "days that is not an array rejects");
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1,\"expected_refresh_s\":3600,"
                     "\"from\":\"2026-11-01\",\"to\":\"2027-02-12\"}",
                     &cal),
              "days absent rejects; empty is the way to say none");
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1,\"from\":\"2026-11-01\","
                     "\"to\":\"2027-02-12\",\"days\":[]}",
                     &cal),
              "a missing cadence rejects");
        CHECK(!PARSE(kdash_parse_calendar,
                     "{\"ts\":1794643200000,\"expected_refresh_s\":3600,"
                     "\"from\":\"2026-11-01\",\"to\":\"2027-02-12\",\"days\":[]}",
                     &cal),
              "a millisecond ts rejects");
        CHECK(cal.days_count == 0 && cal.from[0] == '\0' &&
                  !kdash_calendar_covers(&cal, "2026-11-14"),
              "a rejected record is zeroed and covers nothing");
        CHECK(!kdash_calendar_covers(NULL, "2026-11-14") &&
                  kdash_calendar_day(NULL, "2026-11-14") == NULL &&
                  kdash_calendar_day(&cal, NULL) == NULL,
              "NULL in, false/NULL out");
#undef CAL_HEAD
    }

    /* ---- kdash:agents (CD-31) ---- */
    {
        static kdash_agents_t ag; /* ~20 KB: static, as a card would hold it */
#define AG_HEAD                                                                \
    "{\"ts\":1794700000.5,\"expected_refresh_s\":60,\"window_s\":86400,"
#define AG_BAR(id, start, end, state, ship, ovs, extra)                        \
    "{\"id\":\"" id "\",\"host\":\"kai\",\"name\":\"kdashdata\","             \
    "\"start\":" #start ",\"end\":" #end ",\"state\":\"" state "\","          \
    "\"shipped\":" #ship ",\"overseer\":" #ovs extra "}"

        /* An overseer on cleo with a leg on kai hanging from it, a finished
         * stretch that shipped, and a session waiting on Ken. */
        CHECK(PARSE(kdash_parse_agents,
                    AG_HEAD "\"bars\":["
                    "{\"id\":\"ov1\",\"host\":\"cleo\",\"name\":\"kpidash Agents card\","
                    "\"start\":1794680000,\"end\":1794700000.5,"
                    "\"state\":\"working\",\"shipped\":false,\"overseer\":true},"
                    AG_BAR("leg1", 1794685000, 1794690000, "done", true, false,
                           ",\"parent\":\"ov1\"") ","
                    AG_BAR("s3.b2", 1794695000, 1794700000.5, "waiting", false,
                           false, "") "],"
                    "\"v\":1,\"unknown_future_field\":{\"x\":1}}",
                    &ag),
              "agents record, with `v` and an unknown field ignored");
        CHECK(ag.ts > 1794700000.0 && ag.expected_refresh_s == 60.0 &&
                  ag.window_s == 86400.0,
              "head");
        CHECK(ag.bars_count == 3 && ag.bars_skipped == 0 &&
                  !ag.bars_truncated && ag.parents_dropped == 0,
              "three bars, nothing skipped, cut or dropped");
        CHECK(strcmp(ag.bars[0].host, "cleo") == 0 && ag.bars[0].overseer &&
                  ag.bars[0].state == KDASH_AGENT_WORKING &&
                  ag.bars[0].parent_index == -1 && ag.bars[0].parent[0] == '\0',
              "the overseer bar");
        CHECK(ag.bars[1].shipped && ag.bars[1].state == KDASH_AGENT_DONE &&
                  ag.bars[1].parent_index == 0 &&
                  strcmp(ag.bars[1].parent, "ov1") == 0,
              "the leg hangs from the overseer, index %d",
              ag.bars[1].parent_index);
        CHECK(ag.bars[2].state == KDASH_AGENT_WAITING &&
                  strcmp(ag.bars[2].id, "s3.b2") == 0,
              "the waiting bar");

        /* The right edge. Fresh record: running bars reach now, done bars
         * stop at `end`. Stale record: running bars stop at `end` too. */
        long long now = 1794700030;
        CHECK(kdash_agents_bar_end(&ag, &ag.bars[0], now) == (double)now &&
                  kdash_agents_bar_end(&ag, &ag.bars[2], now) == (double)now,
              "fresh: working and waiting bars reach now");
        CHECK(kdash_agents_bar_end(&ag, &ag.bars[1], now) == 1794690000.0,
              "a done bar ends at its end");
        CHECK(kdash_agents_bar_end(&ag, &ag.bars[0], 1794700000) == 1794700000.0,
              "and an end past the reader's clock is clipped to now");
        now = 1794700000 + 60 + KDASH_CADENCE_GRACE_S + 1;
        CHECK(kdash_agents_bar_end(&ag, &ag.bars[0], now) == 1794700000.5,
              "stale: a running bar stops where the publisher last saw it");
        CHECK(kdash_agents_bar_end(NULL, &ag.bars[0], now) == 0 &&
                  kdash_agents_bar_end(&ag, NULL, now) == 0,
              "NULL in, 0 out");

        /* The state words round-trip. */
        kdash_agent_state_t st;
        CHECK(kdash_agent_state_from_str("waiting", &st) &&
                  st == KDASH_AGENT_WAITING &&
                  strcmp(kdash_agent_state_str(st), "waiting") == 0 &&
                  !kdash_agent_state_from_str("Waiting", &st) &&
                  !kdash_agent_state_from_str("idle", &st),
              "state words, case-sensitive and closed");

        /* No agent ran in the window: a statement, not an absence. */
        CHECK(PARSE(kdash_parse_agents, AG_HEAD "\"bars\":[]}", &ag),
              "empty bars");
        CHECK(ag.bars_count == 0 && ag.bars_skipped == 0 && ag.ts > 0,
              "an empty record is a valid record");

        /* Bad bars are skipped and counted; the record survives them. */
        CHECK(PARSE(kdash_parse_agents,
                    AG_HEAD "\"bars\":["
                    AG_BAR("a", 1794690000, 1794691000, "idle", false, false, "") ","
                    AG_BAR("b", 1794690000, 1794691000, "Done", false, false, "") ","
                    AG_BAR("c", 1794692000, 1794691000, "done", false, false, "") ","
                    AG_BAR("d", 1794600000, 1794613000, "done", false, false, "") ","
                    AG_BAR("abcdefghijklmnopqrstuvwx", 1794690000, 1794691000,
                           "done", false, false, "") ","
                    AG_BAR("e f", 1794690000, 1794691000, "done", false, false, "") ","
                    "{\"id\":\"g\",\"host\":\"a-very-long-host\",\"name\":\"x\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false,\"overseer\":false},"
                    "{\"id\":\"h\",\"host\":\"kai\","
                    "\"name\":\"abcdefghijklmnopqrstuvwxyz012345\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false,\"overseer\":false},"
                    "{\"id\":\"i\",\"host\":\"kai\",\"name\":\"\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false,\"overseer\":false},"
                    AG_BAR("j", 1794690000, 1794691000, "done", "no", false, "") ","
                    "{\"id\":\"k\",\"host\":\"kai\",\"name\":\"x\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false},"
                    AG_BAR("l", 1794690000000, 1794691000000, "done", false,
                           false, "") ","
                    "\"m\","
                    AG_BAR("ok", 1794690000, 1794691000, "done", false, false, "") ","
                    AG_BAR("ok", 1794692000, 1794693000, "done", false, false, "")
                    "]}",
                    &ag),
              "a record with fourteen bad bars and one good one");
        CHECK(ag.bars_count == 1 && ag.bars_skipped == 14 &&
                  strcmp(ag.bars[0].id, "ok") == 0 &&
                  ag.bars[0].start == 1794690000.0,
              "only the first `ok` kept, %d skipped", ag.bars_skipped);

        /* A name is bounded in CHARACTERS, as the schema counts: 31 non-ASCII
         * characters fit, though they are 62 bytes. */
        CHECK(PARSE(kdash_parse_agents,
                    AG_HEAD "\"bars\":[{\"id\":\"u\",\"host\":\"kai\",\"name\":"
                    "\"ééééééééééééééééééééééééééééééé\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false,\"overseer\":false},"
                    "{\"id\":\"v\",\"host\":\"kai\",\"name\":"
                    "\"éééééééééééééééééééééééééééééééé\","
                    "\"start\":1794690000,\"end\":1794691000,\"state\":\"done\","
                    "\"shipped\":false,\"overseer\":false}]}",
                    &ag),
              "two UTF-8 names");
        CHECK(ag.bars_count == 1 && ag.bars_skipped == 1 &&
                  strlen(ag.bars[0].name) == 62,
              "31 characters kept whole, 32 skipped");

        /* A bar that started before the window but ends inside it is kept with
         * its true start; the card clips. One that ended before is skipped. */
        CHECK(PARSE(kdash_parse_agents,
                    AG_HEAD "\"bars\":["
                    AG_BAR("early", 1794600000, 1794614000.5, "done", false,
                           false, "") "]}",
                    &ag),
              "a bar straddling the window's left edge");
        CHECK(ag.bars_count == 1 && ag.bars[0].start == 1794600000.0,
              "kept, start unclipped");

        /* Parent links: dangling, naming a non-overseer, naming itself,
         * malformed and null are all dropped; the bars are kept. A parent
         * that appears LATER in the record still resolves. */
        CHECK(PARSE(kdash_parse_agents,
                    AG_HEAD "\"bars\":["
                    AG_BAR("p1", 1794690000, 1794691000, "done", false, false,
                           ",\"parent\":\"gone\"") ","
                    AG_BAR("p2", 1794690000, 1794691000, "done", false, false,
                           ",\"parent\":\"p1\"") ","
                    AG_BAR("p3", 1794690000, 1794691000, "done", false, true,
                           ",\"parent\":\"p3\"") ","
                    AG_BAR("p4", 1794690000, 1794691000, "done", false, false,
                           ",\"parent\":7") ","
                    AG_BAR("p5", 1794690000, 1794691000, "done", false, false,
                           ",\"parent\":null") ","
                    AG_BAR("p6", 1794690000, 1794691000, "done", false, false,
                           ",\"parent\":\"ov\"") ","
                    AG_BAR("ov", 1794695000, 1794699000, "done", false, true,
                           "") "]}",
                    &ag),
              "a record with five bad parent links and one good one");
        CHECK(ag.bars_count == 7 && ag.bars_skipped == 0 &&
                  ag.parents_dropped == 5,
              "every bar kept, %d links dropped", ag.parents_dropped);
        CHECK(ag.bars[0].parent_index == -1 && ag.bars[0].parent[0] == '\0' &&
                  ag.bars[1].parent_index == -1 &&
                  ag.bars[2].parent_index == -1 &&
                  ag.bars[3].parent_index == -1 &&
                  ag.bars[4].parent_index == -1,
              "a dropped link reads as no parent");
        CHECK(ag.bars[5].parent_index == 6 &&
                  strcmp(ag.bars[5].parent, "ov") == 0,
              "a forward link resolves");

        /* 96 bars, the schema's cap: all kept. 100: the NEWEST 96 kept,
         * because the card's right edge is now. A link to an evicted overseer
         * is dropped. */
        {
            static char big[65536];
            int n = snprintf(big, sizeof(big), AG_HEAD "\"bars\":[");
            for (int i = 0; i < 100; i++)
                n += snprintf(big + n, sizeof(big) - n,
                              "%s{\"id\":\"b%d\",\"host\":\"kai\",\"name\":\"x\","
                              "\"start\":%d,\"end\":%d,\"state\":\"done\","
                              "\"shipped\":false,\"overseer\":%s%s}",
                              i ? "," : "", i, 1794690000 + i, 1794690000 + i,
                              i == 0 ? "true" : "false",
                              i == 99 ? ",\"parent\":\"b0\"" : "");
            n += snprintf(big + n, sizeof(big) - n, "]}");
            CHECK(PARSE(kdash_parse_agents, big, &ag), "100 bars parse");
            CHECK(ag.bars_count == KDASH_AGENTS_BARS_MAX && ag.bars_truncated &&
                      ag.bars_skipped == 0,
                  "96 kept and flagged");
            CHECK(strcmp(ag.bars[0].id, "b4") == 0 &&
                      strcmp(ag.bars[95].id, "b99") == 0,
                  "the newest 96, first kept \"%s\"", ag.bars[0].id);
            CHECK(ag.bars[95].parent_index == -1 && ag.parents_dropped == 1,
                  "a link to an evicted overseer is dropped");

            n = snprintf(big, sizeof(big), AG_HEAD "\"bars\":[");
            for (int i = 0; i < KDASH_AGENTS_BARS_MAX; i++)
                n += snprintf(big + n, sizeof(big) - n,
                              "%s{\"id\":\"b%d\",\"host\":\"kai\",\"name\":\"x\","
                              "\"start\":%d,\"end\":%d,\"state\":\"done\","
                              "\"shipped\":false,\"overseer\":false}",
                              i ? "," : "", i, 1794690000 + i, 1794690000 + i);
            n += snprintf(big + n, sizeof(big) - n, "]}");
            CHECK(PARSE(kdash_parse_agents, big, &ag) &&
                      ag.bars_count == KDASH_AGENTS_BARS_MAX &&
                      !ag.bars_truncated,
                  "exactly 96 bars: all kept, not flagged");
        }

        /* Record-level rejections. */
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"bars\":[]}", &ag),
              "window_s absent rejects");
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"window_s\":60,"
                     "\"bars\":[]}",
                     &ag),
              "a window under an hour rejects");
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"window_s\":86400.5,"
                     "\"bars\":[]}",
                     &ag),
              "a fractional window rejects");
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1,\"expected_refresh_s\":60,\"window_s\":86400}",
                     &ag),
              "bars absent rejects; empty is the way to say none");
        CHECK(!PARSE(kdash_parse_agents, AG_HEAD "\"bars\":{}}", &ag),
              "bars that is not an array rejects");
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1,\"window_s\":86400,\"bars\":[]}", &ag),
              "a missing cadence rejects");
        CHECK(!PARSE(kdash_parse_agents,
                     "{\"ts\":1794700000000,\"expected_refresh_s\":60,"
                     "\"window_s\":86400,\"bars\":[]}",
                     &ag),
              "a millisecond ts rejects");
        CHECK(ag.bars_count == 0 && ag.ts == 0 && ag.window_s == 0,
              "a rejected record is zeroed");
#undef AG_BAR
#undef AG_HEAD
    }

    return TEST_RESULT();
}
