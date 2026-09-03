/**
 * @file kdash_dump.c
 * The toy consumer from WI #1743's acceptance criterion: render every schema'd
 * family from its own home, knowing no key grammar, no SCAN, and no hiredis.
 * It is also the only exercise the khlenv socket path and the connection
 * handle get outside a live homelab — run it on any fleet host:
 *
 *   ./build/kdash_dump                       # discover the endpoints via khlenv
 *   KDASH_CENTRAL_REDIS=rpi53:6379 ./build/kdash_dump    # pin the central one
 *   KDASH_CLAUDE_REDIS=rpi53:6379 ./build/kdash_dump     # pin the claude one
 *   KDASH_PANEL_HOST=kstudio ./build/kdash_dump          # another panel's command
 *
 * TWO handles, two stems (CD-4/CD-7): the kpidash families resolve
 * KDASH_CENTRAL_REDIS and the claude family resolves KDASH_CLAUDE_REDIS. They
 * answer the same host:port today and are still kept apart here on purpose —
 * a dump that quietly read `claude:*` through the central stem would look
 * right and prove nothing. An argv[1] endpoint override applies to the central
 * handle only, for the same reason; pin the claude one through its own stem.
 *
 * REDISCLI_AUTH supplies the password (CD-2). Read-only throughout (CD-5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "kdash/kdash.h"

#define MAX_HOSTS    32
#define MAX_SERVICES 64
#define MAX_ZONES    16
#define MAX_SESSIONS 32
#define MAX_RECENT   20

static void print_endpoint(kdash_conn_t *c, const char *stem) {
    char host[KDASH_ENDPOINT_MAX] = "";
    int port = 0;
    /* Force a resolve+connect so the endpoint below is the one actually in
     * use rather than the handle's uninitialised guess. */
    bool up = kdash_conn_ensure(c);
    kdash_endpoint_status_t st = kdash_conn_endpoint(c, host, sizeof(host), &port);

    switch (st) {
    case KDASH_EP_NONE:
        printf("%-20s khlenv holds an explicit null — deliberately none\n", stem);
        return;
    case KDASH_EP_INVALID:
        printf("%-20s resolved to something unusable\n", stem);
        return;
    case KDASH_EP_UNRESOLVED:
        /* This stem has no compiled-in default, so it would rather resolve
         * nothing than send a dashboard to a guess (CD-4). */
        printf("%-20s unresolved — khlenv answered nothing and this stem has "
               "no default\n",
               stem);
        return;
    case KDASH_EP_OK:
        printf("%-20s %s:%d (%s)\n", stem, host, port,
               up ? "connected" : "unreachable");
        return;
    }
}

static void dump_clients(kdash_conn_t *c, long long now) {
    char hosts[MAX_HOSTS][KDASH_TOKEN_MAX];
    int skipped = 0;
    int n = kdash_clients(c, hosts, MAX_HOSTS, &skipped);

    printf("\n== clients (kpidash:clients) ==\n");
    if (n < 0) {
        printf("  unavailable\n");
        return;
    }
    printf("  %d host(s)%s\n", n, skipped ? " (some members skipped)" : "");

    for (int i = 0; i < n; i++) {
        printf("  %-16s", hosts[i]);

        /* The expiring feeds answer freshness by absence: a present key is a
         * live publisher, full stop. No window arithmetic. */
        kdash_health_t h;
        switch (kdash_health(c, hosts[i], &h)) {
        case KDASH_OK:
            printf(" health=up(%llds ago)", (long long)kdash_age_s(h.last_seen_ts, now));
            break;
        case KDASH_ABSENT:
            printf(" health=offline");
            break;
        case KDASH_UNAVAIL:
            printf(" health=?");
            break;
        }

        kdash_telemetry_t t;
        if (kdash_telemetry(c, hosts[i], &t) == KDASH_OK) {
            printf("  cpu=%.0f%% ram=%.0f/%.0fMB", t.cpu_pct, t.ram_used_mb,
                   t.ram_total_mb);
            if (t.has_gpu)
                printf(" gpu=%.0f%%", t.gpu_compute_pct);
            if (t.ndisks)
                printf(" disks=%d", t.ndisks);
        }

        kdash_dev_telemetry_t d;
        if (kdash_dev_telemetry(c, hosts[i], &d) == KDASH_OK)
            printf("  dev=%.0f%%", d.cpu_pct);

        printf("\n");
    }
}

static void dump_services(kdash_conn_t *c, long long now) {
    kdash_service_t svcs[MAX_SERVICES];
    int skipped = 0;
    int n = kdash_services(c, svcs, MAX_SERVICES, &skipped);

    printf("\n== services (kpidash:services:*:*) ==\n");
    if (n < 0) {
        printf("  unavailable\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        /* ts-owned: the reader owns the window, so the panel decides what
         * stale looks like. */
        bool stale = kdash_ts_stale(svcs[i].ts, now, KDASH_SERVICES_WINDOW_S);
        printf("  %-20s %-10s %-9s %s%s\n", svcs[i].name,
               svcs[i].host[0] ? svcs[i].host : "(no host)",
               kdash_service_state_str(svcs[i].state), svcs[i].text,
               stale ? "  [stale]" : "");
    }
    printf("  %d card(s), %d skipped\n", n, skipped);
}

static void dump_apttemps(kdash_conn_t *c, long long now) {
    kdash_apttemps_t zones[MAX_ZONES];
    int skipped = 0;
    int n = kdash_apttemps(c, zones, MAX_ZONES, &skipped);

    printf("\n== apartment temperatures (kpidash:apttemps:*) ==\n");
    if (n < 0) {
        printf("  unavailable\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        bool stale = kdash_ts_stale(zones[i].ts, now, KDASH_APTTEMPS_WINDOW_S);
        printf("  %-12s %5.1f F  %3.0f%% RH%s\n",
               zones[i].label[0] ? zones[i].label : zones[i].zone,
               zones[i].temp_f, zones[i].humidity_pct, stale ? "  [stale]" : "");
    }
    printf("  %d zone(s), %d skipped\n", n, skipped);
}

/* kdash:panel:<host> — the control feed (CD-17). Addressed per panel with no
 * discovery set to walk, so this reads the key with THIS host's name on it,
 * which is what a real dashboard does. `KDASH_PANEL_HOST` looks at another
 * panel's instead — the useful case from a build host, since the panel being
 * commanded is usually not the one you are sitting at. */
static void dump_panel(kdash_conn_t *c, long long now) {
    char host[KDASH_TOKEN_MAX] = "";
    const char *pinned = getenv("KDASH_PANEL_HOST");
    if (pinned && pinned[0]) {
        snprintf(host, sizeof(host), "%s", pinned);
    } else if (gethostname(host, sizeof(host)) != 0) {
        host[0] = '\0';
    }
    /* gethostname() may not terminate on truncation. */
    host[sizeof(host) - 1] = '\0';

    printf("\n== panel control (kdash:panel:%s) ==\n", host[0] ? host : "?");
    if (!host[0]) {
        printf("  no host to ask about\n");
        return;
    }

    kdash_panel_t p;
    switch (kdash_panel(c, host, &p)) {
    case KDASH_UNAVAIL:
        printf("  unavailable\n");
        return;
    case KDASH_ABSENT:
        /* Distinct from unavailable on purpose: nobody has ever commanded this
         * panel, which is not a fault and not a reason to render anything. */
        printf("  no command for this panel\n");
        return;
    case KDASH_OK:
        /* acted_ts 0 — a fresh process has acted on nothing, which is exactly
         * the state a panel is in when it boots. */
        printf("  want=%s  issued %llds ago%s\n", kdash_panel_want_str(p.want),
               kdash_age_s(p.ts, now),
               kdash_panel_actionable(&p, 0, now, KDASH_PANEL_WINDOW_S)
                   ? "  [a booting panel would act on this]"
                   : "  [outside the window — a booting panel would ignore it]");
        return;
    }
}

/* The claude family, on its own handle at its own stem. Three feeds, two of
 * them Redis HASHes rather than JSON documents — which the consumer never has
 * to know, because the readers hand back the same kind of struct either way. */
static void dump_claude(kdash_conn_t *c, long long now) {
    kdash_claude_session_t sessions[MAX_SESSIONS];
    int skipped = 0;
    int n = kdash_claude_sessions(c, sessions, MAX_SESSIONS, &skipped);

    printf("\n== claude sessions (claude:session:*) ==\n");
    if (n < 0) {
        printf("  unavailable\n");
    } else {
        /* Derive display state and order the rows. The library answers "which
         * of these needs me most"; what that looks like on a screen is the
         * panel's business (CD-16). */
        kdash_claude_sessions_refresh(sessions, n, now, KDASH_CLAUDE_IDLE_S,
                                      KDASH_CLAUDE_STALE_S);
        for (int i = 0; i < n; i++) {
            const kdash_claude_session_t *s = &sessions[i];
            printf("  %-9s %-12s %-16s %llds ago", kdash_claude_disp_str(s->disp),
                   s->host, s->project[0] ? s->project : "(none)",
                   kdash_age_s(s->ts, now));
            if (s->model[0])
                printf("  %s", s->model);
            if (s->title[0])
                printf("  \"%s\"", s->title);
            printf("\n");
        }
        printf("  %d session(s), %d skipped\n", n, skipped);
    }

    printf("\n== claude usage limits (claude:limits) ==\n");
    kdash_claude_limits_t l;
    switch (kdash_claude_limits(c, &l)) {
    case KDASH_UNAVAIL:
        printf("  unavailable\n");
        break;
    case KDASH_ABSENT:
        printf("  nobody has published usage\n");
        break;
    case KDASH_OK: {
        /* Each gauge is judged against its OWN stamp — the whole point of the
         * scoped set carrying one. */
        bool stale = kdash_claude_limits_stale(&l, now, KDASH_CLAUDE_LIMITS_GRACE_S,
                                               KDASH_CLAUDE_LIMITS_STALE_S);
        printf("  five-hour %5.1f%%   seven-day %5.1f%%   (observed %llds ago%s)\n",
               l.five_hour_pct, l.seven_day_pct,
               kdash_age_s(l.updated_at, now), stale ? ", STALE" : "");
        if (l.scoped_valid) {
            bool sstale = kdash_claude_limits_scoped_stale(
                &l, now, KDASH_CLAUDE_LIMITS_GRACE_S, KDASH_CLAUDE_LIMITS_STALE_S);
            printf("  scoped %-8s %5.1f%%%s   (observed %llds ago%s)\n",
                   l.scoped_model, l.scoped_pct, l.scoped_active ? " *" : "  ",
                   kdash_age_s(l.scoped_updated_at, now), sstale ? ", STALE" : "");
        } else {
            printf("  no model-scoped window published\n");
        }
        break;
    }
    }

    kdash_claude_recent_t recent[MAX_RECENT];
    skipped = 0;
    n = kdash_claude_recent(c, recent, MAX_RECENT, &skipped);

    printf("\n== claude recent (claude:recent) ==\n");
    if (n < 0) {
        printf("  unavailable\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        printf("  %-12s %-16s %llds ago", recent[i].host, recent[i].project,
               kdash_age_s(recent[i].ended_ts, now));
        if (recent[i].dur_s > 0)
            printf("  ran %llds", (long long)recent[i].dur_s);
        if (recent[i].title[0])
            printf("  \"%s\"", recent[i].title);
        printf("\n");
    }
    printf("  %d entry(s), %d skipped\n", n, skipped);
}

int main(int argc, char **argv) {
    kdash_conn_opts_t opts = {.app = "kdash-dump"}; /* .stem NULL -> central */
    if (argc > 1)
        opts.host = argv[1]; /* explicit endpoint override, for a quick probe */

    kdash_conn_t *c = kdash_conn_new(&opts);
    /* The claude family's own handle. Independent by construction, so a claude
     * endpoint that is down costs the kpidash half of this dump nothing. */
    kdash_conn_opts_t claude_opts = {.app = "kdash-dump",
                                     .stem = &KDASH_STEM_CLAUDE};
    kdash_conn_t *cc = kdash_conn_new(&claude_opts);
    if (!c || !cc) {
        fprintf(stderr, "kdash_conn_new failed\n");
        kdash_conn_free(c);
        kdash_conn_free(cc);
        return 1;
    }

    long long now = (long long)time(NULL);
    print_endpoint(c, KDASH_CENTRAL_STEM);
    print_endpoint(cc, KDASH_CLAUDE_STEM);
    dump_clients(c, now);
    dump_services(c, now);
    dump_apttemps(c, now);
    dump_panel(c, now);
    dump_claude(cc, now);

    /* A dashboard would render "unavailable" here and carry on; a CLI can be
     * blunter about it. Either way nothing crashed and nothing blocked. */
    int rc = (kdash_conn_reachable(c) && kdash_conn_reachable(cc)) ? 0 : 2;
    kdash_conn_free(c);
    kdash_conn_free(cc);
    return rc;
}
