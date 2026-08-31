/**
 * @file test_endpoint.c
 * The pure half of endpoint discovery: host:port parsing, the khlenv endpoint
 * file, the request it builds, and the three status codes its protocol
 * actually uses. No socket is opened here — the I/O half is verified live
 * against the running khlenv, and a unit test that needed a server would not
 * be one `just check` could run.
 */
#include <string.h>

#include "check.h"
#include "kdash/kdash_endpoint.h"

static void hostport_ok(const char *value, int defport, const char *want_host,
                        int want_port) {
    char host[KDASH_ENDPOINT_MAX];
    int port = 0;
    bool got = kdash_parse_hostport(value, defport, host, sizeof(host), &port);
    CHECK(got, "expected accept: \"%s\"", value);
    if (got) {
        CHECK(strcmp(host, want_host) == 0, "\"%s\" -> host \"%s\", want \"%s\"",
              value, host, want_host);
        CHECK(port == want_port, "\"%s\" -> port %d, want %d", value, port,
              want_port);
    }
}

static void hostport_reject(const char *value) {
    char host[KDASH_ENDPOINT_MAX];
    int port = 0;
    CHECK(!kdash_parse_hostport(value, 6379, host, sizeof(host), &port),
          "expected reject: \"%s\"", value);
}

static void endpoint_ok(const char *text, const char *want_host, int want_port) {
    char host[KDASH_ENDPOINT_MAX];
    int port = 0;
    bool got =
        kdash_khlenv_parse_endpoint(text, strlen(text), host, sizeof(host), &port);
    CHECK(got, "expected accept: \"%s\"", text);
    if (got) {
        CHECK(strcmp(host, want_host) == 0, "\"%s\" -> \"%s\", want \"%s\"", text,
              host, want_host);
        CHECK(port == want_port, "\"%s\" -> port %d, want %d", text, port,
              want_port);
    }
}

int main(void) {
    /* ---- host:port ---- */
    hostport_ok("rpi53:6379", 6379, "rpi53", 6379);
    hostport_ok("rpi53", 6379, "rpi53", 6379);          /* default port */
    hostport_ok("rpidash2:6380", 6379, "rpidash2", 6380);
    hostport_ok("127.0.0.1:6379", 6379, "127.0.0.1", 6379);
    hostport_ok("  rpi53:6379\n", 6379, "rpi53", 6379); /* body whitespace */
    hostport_ok(KDASH_CENTRAL_DEFAULT, 6379, "rpi53", 6379);

    hostport_reject("");
    hostport_reject(":6379");         /* no host */
    hostport_reject("rpi53:");        /* no port after the colon */
    hostport_reject("rpi53:abc");     /* non-numeric port */
    hostport_reject("rpi53:0");       /* out of range */
    hostport_reject("rpi53:65536");
    hostport_reject("rpi53:99999999");
    /* An IPv6 literal would be mis-split on its own colons. Refusing it is
     * honest; the homelab addresses Redis by name or IPv4. */
    hostport_reject("::1");
    hostport_reject("fd00::1:6379");

    /* ---- the khlenv endpoint file ---- */
    endpoint_ok("http://kubs0.encke-wahoo.ts.net:7770", "kubs0.encke-wahoo.ts.net",
                7770);
    endpoint_ok("http://kubs0:7770\n", "kubs0", 7770);   /* trailing newline */
    endpoint_ok("http://kubs0:7770/", "kubs0", 7770);    /* trailing slash */
    endpoint_ok("http://kubs0:7770/v1", "kubs0", 7770);  /* trailing path */
    endpoint_ok("kubs0:7770", "kubs0", 7770);            /* scheme optional */
    endpoint_ok("http://kubs0", "kubs0", 80);            /* http default port */

    {
        char host[KDASH_ENDPOINT_MAX];
        int port = 0;
        CHECK(!kdash_khlenv_parse_endpoint("", 0, host, sizeof(host), &port),
              "empty endpoint file");
        CHECK(!kdash_khlenv_parse_endpoint("\n\n", 2, host, sizeof(host), &port),
              "whitespace-only endpoint file");
        /* khlenv is http-only by design — it holds no secrets, so there is no
         * TLS client here to fall back to, and quietly treating https:// as
         * http:// would connect to the wrong port. */
        CHECK(!kdash_khlenv_parse_endpoint("https://kubs0:7770", 18, host,
                                           sizeof(host), &port),
              "https is not supported and must not be guessed at");
    }

    /* ---- the request ---- */
    {
        char req[512];
        CHECK(kdash_khlenv_build_request(req, sizeof(req), "kubs0", 7770,
                                         "kstudiodash", KDASH_CENTRAL_STEM),
              "build a resolve request");
        CHECK(strstr(req, "GET /v1/resolve?app=kstudiodash&key=KDASH_CENTRAL_REDIS "
                          "HTTP/1.1\r\n") == req,
              "request line: %.60s", req);
        CHECK(strstr(req, "\r\nHost: kubs0:7770\r\n") != NULL, "Host header");
        CHECK(strstr(req, "\r\nConnection: close\r\n") != NULL,
              "Connection: close — the read loop reads to EOF");
        CHECK(strstr(req, "\r\n\r\n") != NULL, "header terminator");

        /* app and key are validated, not escaped: the token charset reserves
         * nothing in a query string, so anything outside it is refused rather
         * than encoded — a tighter choke point and less code. */
        CHECK(!kdash_khlenv_build_request(req, sizeof(req), "kubs0", 7770,
                                          "app&key=X", KDASH_CENTRAL_STEM),
              "an app that would forge a parameter is refused");
        CHECK(!kdash_khlenv_build_request(req, sizeof(req), "kubs0", 7770, "app",
                                          "KEY WITH SPACE"),
              "a key with a space is refused");
        CHECK(!kdash_khlenv_build_request(req, sizeof(req), "kubs0", 7770, "",
                                          KDASH_CENTRAL_STEM),
              "an empty app is refused");
        char tiny[16];
        CHECK(!kdash_khlenv_build_request(tiny, sizeof(tiny), "kubs0", 7770, "a",
                                          "B"),
              "a buffer too small fails rather than sending half a request");
    }

    /* ---- the response ---- */
    {
        char v[KDASH_ENDPOINT_MAX];
        const char ok200[] =
            "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
            "X-Khlenv-Stem: KDASH_CENTRAL_REDIS.kai\r\n"
            "Content-Length: 10\r\n\r\nrpi53:6379";
        CHECK(kdash_khlenv_parse_response(ok200, strlen(ok200), v, sizeof(v)) ==
                  KDASH_KHLENV_OK,
              "200 is a value");
        CHECK(strcmp(v, "rpi53:6379") == 0, "body \"%s\"", v);

        const char ok_nl[] = "HTTP/1.1 200 OK\r\n\r\nrpi53:6379\n";
        CHECK(kdash_khlenv_parse_response(ok_nl, strlen(ok_nl), v, sizeof(v)) ==
                  KDASH_KHLENV_OK,
              "a trailing newline in the body is trimmed");
        CHECK(strcmp(v, "rpi53:6379") == 0, "trimmed body \"%s\"", v);

        /* 204 is an explicit null: "deliberately no endpoint". It is an
         * ANSWER, and the resolver must not fall back past it. */
        const char no204[] = "HTTP/1.1 204 No Content\r\n\r\n";
        CHECK(kdash_khlenv_parse_response(no204, strlen(no204), v, sizeof(v)) ==
                  KDASH_KHLENV_NULL,
              "204 is an explicit null");

        const char miss[] = "HTTP/1.1 404 Not Found\r\n\r\ntried: A -> B -> C";
        CHECK(kdash_khlenv_parse_response(miss, strlen(miss), v, sizeof(v)) ==
                  KDASH_KHLENV_MISS,
              "404 is a miss");

        const char err[] = "HTTP/1.1 500 Internal Server Error\r\n\r\nboom";
        CHECK(kdash_khlenv_parse_response(err, strlen(err), v, sizeof(v)) ==
                  KDASH_KHLENV_UNAVAIL,
              "500 is unavailable, not a value");

        /* A 200 carrying nothing is not a value — adopting "" as an endpoint
         * would be worse than reporting the service unusable. */
        const char empty[] = "HTTP/1.1 200 OK\r\n\r\n";
        CHECK(kdash_khlenv_parse_response(empty, strlen(empty), v, sizeof(v)) ==
                  KDASH_KHLENV_UNAVAIL,
              "200 with an empty body");
        const char nohdr[] = "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n";
        CHECK(kdash_khlenv_parse_response(nohdr, strlen(nohdr), v, sizeof(v)) ==
                  KDASH_KHLENV_UNAVAIL,
              "headers with no terminator");
        CHECK(kdash_khlenv_parse_response("garbage", 7, v, sizeof(v)) ==
                  KDASH_KHLENV_UNAVAIL,
              "not an HTTP response");
        CHECK(kdash_khlenv_parse_response("", 0, v, sizeof(v)) ==
                  KDASH_KHLENV_UNAVAIL,
              "empty response");

        /* HTTP/1.0 is still HTTP. */
        const char v10[] = "HTTP/1.0 200 OK\r\n\r\nrpi53:6379";
        CHECK(kdash_khlenv_parse_response(v10, strlen(v10), v, sizeof(v)) ==
                  KDASH_KHLENV_OK,
              "HTTP/1.0 response");

        /* A value longer than the caller's buffer fails rather than
         * truncating an endpoint into a different one. */
        char smallv[8];
        CHECK(kdash_khlenv_parse_response(ok200, strlen(ok200), smallv,
                                          sizeof(smallv)) == KDASH_KHLENV_UNAVAIL,
              "value too long for the buffer");
    }

    /* The contract's constants, pinned so a change has to come through here. */
    CHECK(strcmp(KDASH_CENTRAL_STEM, "KDASH_CENTRAL_REDIS") == 0, "CD-4 stem");
    CHECK(strcmp(KDASH_CENTRAL_STEM_LEGACY, "KPIDASH_REDIS") == 0, "legacy alias");
    CHECK(strcmp(KDASH_AUTH_ENV, "REDISCLI_AUTH") == 0, "CD-2 auth variable");
    CHECK(KDASH_REDIS_PORT_DEFAULT == 6379, "default Redis port");

    return TEST_RESULT();
}
