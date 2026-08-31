/**
 * @file kdash_dump.c
 * The toy consumer from WI #1743's acceptance criterion: render every schema'd
 * family from the central Redis, knowing no key grammar, no SCAN, and no
 * hiredis. It is also the only exercise the khlenv socket path and the
 * connection handle get outside a live homelab — run it on any fleet host:
 *
 *   ./build/kdash_dump                       # discover the endpoint via khlenv
 *   KDASH_CENTRAL_REDIS=rpi53:6379 ./build/kdash_dump    # pin it
 *
 * REDISCLI_AUTH supplies the password (CD-2). Read-only throughout (CD-5).
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "kdash/kdash.h"

#define MAX_HOSTS    32
#define MAX_SERVICES 64
#define MAX_ZONES    16

static void print_endpoint(kdash_conn_t *c) {
    char host[KDASH_ENDPOINT_MAX] = "";
    int port = 0;
    /* Force a resolve+connect so the endpoint below is the one actually in
     * use rather than the handle's uninitialised guess. */
    bool up = kdash_conn_ensure(c);
    kdash_endpoint_status_t st = kdash_conn_endpoint(c, host, sizeof(host), &port);

    switch (st) {
    case KDASH_EP_NONE:
        printf("endpoint: khlenv holds an explicit null — deliberately none\n");
        return;
    case KDASH_EP_INVALID:
        printf("endpoint: resolved to something unusable\n");
        return;
    case KDASH_EP_OK:
        printf("endpoint: %s:%d (%s)\n", host, port,
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

int main(int argc, char **argv) {
    kdash_conn_opts_t opts = {.app = "kdash-dump"};
    if (argc > 1)
        opts.host = argv[1]; /* explicit endpoint override, for a quick probe */

    kdash_conn_t *c = kdash_conn_new(&opts);
    if (!c) {
        fprintf(stderr, "kdash_conn_new failed\n");
        return 1;
    }

    long long now = (long long)time(NULL);
    print_endpoint(c);
    dump_clients(c, now);
    dump_services(c, now);
    dump_apttemps(c, now);

    /* A dashboard would render "unavailable" here and carry on; a CLI can be
     * blunter about it. Either way nothing crashed and nothing blocked. */
    int rc = kdash_conn_reachable(c) ? 0 : 2;
    kdash_conn_free(c);
    return rc;
}
