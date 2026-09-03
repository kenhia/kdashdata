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
