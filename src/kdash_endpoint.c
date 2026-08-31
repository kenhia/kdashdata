/**
 * @file kdash_endpoint.c
 * khlenv protocol + CD-4 central-endpoint resolution.
 *
 * The parsing half (host:port, endpoint URL, HTTP response) is pure and
 * host-tested. The socket half is one bounded-timeout plain-HTTP GET: no TLS,
 * no redirects, no keep-alive, no chunked encoding — khlenv's protocol is
 * deliberately dumb enough that `curl -i` is its whole debugging story, and
 * this is the C spelling of that same curl.
 */
#include "kdash/kdash_endpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "kdash/kdash_keys.h"

/* A resolve sits on the reconnect path, so it gets the same discipline as the
 * Redis connect: short, bounded, and never a reason to stall a render. */
#define KHLENV_CONNECT_TIMEOUT_MS 250
#define KHLENV_READ_TIMEOUT_MS    400
#define KHLENV_RESPONSE_MAX       2048
#define KHLENV_REQUEST_MAX        512

/* ---- pure ---------------------------------------------------------------- */

bool kdash_parse_hostport(const char *value, int defport, char *host,
                          size_t hostsz, int *port) {
    if (!value || !host || !port || hostsz == 0)
        return false;

    /* Trim surrounding whitespace; khlenv values arrive from a file or a
     * response body and may carry a newline. */
    while (*value == ' ' || *value == '\t')
        value++;
    size_t len = strlen(value);
    while (len > 0 && (value[len - 1] == '\n' || value[len - 1] == '\r' ||
                       value[len - 1] == ' ' || value[len - 1] == '\t'))
        len--;
    if (len == 0)
        return false;

    const char *colon = memchr(value, ':', len);
    size_t hlen = colon ? (size_t)(colon - value) : len;
    if (hlen == 0 || hlen + 1 > hostsz)
        return false;
    /* Two colons means an IPv6 literal (or a typo); refuse rather than split
     * it wrongly. */
    if (colon && memchr(colon + 1, ':', len - hlen - 1))
        return false;

    int p = defport;
    if (colon) {
        size_t plen = len - hlen - 1;
        if (plen == 0 || plen > 5)
            return false;
        p = 0;
        for (size_t i = 0; i < plen; i++) {
            char c = colon[1 + i];
            if (c < '0' || c > '9')
                return false;
            p = p * 10 + (c - '0');
        }
        if (p < 1 || p > 65535)
            return false;
    }

    memcpy(host, value, hlen);
    host[hlen] = '\0';
    *port = p;
    return true;
}

bool kdash_khlenv_parse_endpoint(const char *text, size_t len, char *host,
                                 size_t hostsz, int *port) {
    if (!text || len == 0 || !host || !port)
        return false;

    /* Trim leading/trailing whitespace around the single line. */
    while (len > 0 && (*text == ' ' || *text == '\t' || *text == '\n' ||
                       *text == '\r')) {
        text++;
        len--;
    }
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r' ||
                       text[len - 1] == ' ' || text[len - 1] == '\t'))
        len--;
    if (len == 0)
        return false;

    static const char SCHEME[] = "http://";
    const size_t schemelen = sizeof(SCHEME) - 1;
    if (len > schemelen && memcmp(text, SCHEME, schemelen) == 0) {
        text += schemelen;
        len -= schemelen;
    } else if (memchr(text, '/', len) != NULL) {
        /* Some other scheme, or a bare path — not something to guess at.
         * khlenv is http-only by design (it holds no secrets). */
        const char *slash = memchr(text, '/', len);
        if (slash == text || (slash > text && slash[-1] == ':'))
            return false;
    }

    /* Drop any trailing path: `http://host:7770/` and `.../v1` both name the
     * same service. */
    const char *slash = memchr(text, '/', len);
    if (slash)
        len = (size_t)(slash - text);
    if (len == 0)
        return false;

    char buf[KDASH_ENDPOINT_MAX];
    if (len + 1 > sizeof(buf))
        return false;
    memcpy(buf, text, len);
    buf[len] = '\0';
    return kdash_parse_hostport(buf, 80, host, hostsz, port);
}

bool kdash_khlenv_build_request(char *out, size_t outsz, const char *host,
                                int port, const char *app, const char *key) {
    if (!out || !host || !app || !key || outsz == 0)
        return false;
    /* app and key ride the same charset as every other token in this repo, and
     * that charset contains nothing a query string reserves — so validating is
     * both the simpler and the safer choice over escaping. */
    if (!kdash_token_ok(app, strlen(app)) || !kdash_token_ok(key, strlen(key)))
        return false;
    if (port < 1 || port > 65535)
        return false;

    int n = snprintf(out, outsz,
                     "GET /v1/resolve?app=%s&key=%s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: kdash/0\r\n"
                     "Accept: */*\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     app, key, host, port);
    return n > 0 && (size_t)n < outsz;
}

kdash_khlenv_status_t kdash_khlenv_parse_response(const char *buf, size_t len,
                                                  char *value, size_t valuesz) {
    if (!buf || len < 12 || !value || valuesz == 0)
        return KDASH_KHLENV_UNAVAIL;
    if (memcmp(buf, "HTTP/1.", 7) != 0)
        return KDASH_KHLENV_UNAVAIL;

    /* "HTTP/1.x SSS ..." — the code sits at a fixed offset. */
    const char *sp = memchr(buf, ' ', len);
    if (!sp || (size_t)(sp - buf) + 4 > len)
        return KDASH_KHLENV_UNAVAIL;
    int code = 0;
    for (int i = 1; i <= 3; i++) {
        char c = sp[i];
        if (c < '0' || c > '9')
            return KDASH_KHLENV_UNAVAIL;
        code = code * 10 + (c - '0');
    }

    if (code == 204)
        return KDASH_KHLENV_NULL;
    if (code == 404)
        return KDASH_KHLENV_MISS;
    if (code != 200)
        return KDASH_KHLENV_UNAVAIL;

    /* Body starts after the blank line. */
    const char *body = NULL;
    size_t bodylen = 0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' &&
            buf[i + 3] == '\n') {
            body = buf + i + 4;
            bodylen = len - i - 4;
            break;
        }
    }
    if (!body)
        return KDASH_KHLENV_UNAVAIL;

    while (bodylen > 0 && (*body == ' ' || *body == '\n' || *body == '\r' ||
                           *body == '\t')) {
        body++;
        bodylen--;
    }
    while (bodylen > 0 &&
           (body[bodylen - 1] == '\n' || body[bodylen - 1] == '\r' ||
            body[bodylen - 1] == ' ' || body[bodylen - 1] == '\t'))
        bodylen--;

    /* A 200 with nothing in it is not a value. Saying UNAVAIL rather than OK
     * keeps the caller from adopting "" as an endpoint. */
    if (bodylen == 0 || bodylen + 1 > valuesz)
        return KDASH_KHLENV_UNAVAIL;

    memcpy(value, body, bodylen);
    value[bodylen] = '\0';
    return KDASH_KHLENV_OK;
}

/* ---- I/O ----------------------------------------------------------------- */

/* Read the endpoint URL: the env override first (tests, and hosts with no
 * /etc/khlenv), then the file. */
static bool khlenv_endpoint(char *host, size_t hostsz, int *port) {
    const char *env = getenv(KDASH_KHLENV_ENDPOINT_ENV);
    if (env && env[0])
        return kdash_khlenv_parse_endpoint(env, strlen(env), host, hostsz, port);

    FILE *f = fopen(KDASH_KHLENV_ENDPOINT_FILE, "r");
    if (!f)
        return false;
    char line[KDASH_ENDPOINT_MAX] = {0};
    size_t n = fread(line, 1, sizeof(line) - 1, f);
    fclose(f);
    if (n == 0)
        return false;
    return kdash_khlenv_parse_endpoint(line, n, host, hostsz, port);
}

/* Connect with a bounded timeout. connect(2) honours no timeout of its own, so
 * this goes non-blocking + poll — the same reason kdeskdash caches resolved
 * IPs: a single-threaded dashboard cannot afford an unbounded stall. */
static int connect_bounded(const char *host, int port, int timeout_ms) {
    char portstr[8];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
        return -1;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0)
            break;
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (poll(&pfd, 1, timeout_ms) != 1) {
            close(fd);
            fd = -1;
            continue;
        }
        int err = 0;
        socklen_t errlen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0 || err != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        break;
    }
    freeaddrinfo(res);
    return fd;
}

/* Write the whole request under the same deadline as the read. */
static bool write_all(int fd, const char *buf, size_t len, int timeout_ms) {
    size_t sent = 0;
    while (sent < len) {
        struct pollfd pfd = {.fd = fd, .events = POLLOUT};
        if (poll(&pfd, 1, timeout_ms) != 1)
            return false;
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return false;
    }
    return true;
}

kdash_khlenv_status_t kdash_khlenv_resolve(const char *app, const char *key,
                                           char *value, size_t valuesz) {
    if (!app || !key || !value || valuesz == 0)
        return KDASH_KHLENV_UNAVAIL;

    char host[KDASH_ENDPOINT_MAX];
    int port = 0;
    if (!khlenv_endpoint(host, sizeof(host), &port))
        return KDASH_KHLENV_UNAVAIL;

    char req[KHLENV_REQUEST_MAX];
    if (!kdash_khlenv_build_request(req, sizeof(req), host, port, app, key))
        return KDASH_KHLENV_UNAVAIL;

    int fd = connect_bounded(host, port, KHLENV_CONNECT_TIMEOUT_MS);
    if (fd < 0)
        return KDASH_KHLENV_UNAVAIL;

    kdash_khlenv_status_t st = KDASH_KHLENV_UNAVAIL;
    if (write_all(fd, req, strlen(req), KHLENV_READ_TIMEOUT_MS)) {
        char resp[KHLENV_RESPONSE_MAX];
        size_t got = 0;
        /* Connection: close, so read to EOF — bounded by the buffer and by one
         * poll timeout per read. A response longer than the buffer is a khlenv
         * that stopped being dumb, and is not something to half-parse. */
        while (got < sizeof(resp)) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            if (poll(&pfd, 1, KHLENV_READ_TIMEOUT_MS) != 1)
                break;
            ssize_t n = recv(fd, resp + got, sizeof(resp) - got, 0);
            if (n > 0) {
                got += (size_t)n;
                continue;
            }
            if (n == 0)
                break; /* EOF: the whole response is in hand */
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            break;
        }
        if (got > 0)
            st = kdash_khlenv_parse_response(resp, got, value, valuesz);
    }
    close(fd);
    return st;
}

kdash_endpoint_status_t kdash_resolve_central(const char *app, char *host,
                                              size_t hostsz, int *port) {
    if (!host || !port || hostsz == 0)
        return KDASH_EP_INVALID;

    /* 1. An explicit environment override wins outright. */
    const char *env = getenv(KDASH_CENTRAL_STEM);
    if (env && env[0]) {
        return kdash_parse_hostport(env, KDASH_REDIS_PORT_DEFAULT, host, hostsz,
                                    port)
                   ? KDASH_EP_OK
                   : KDASH_EP_INVALID;
    }

    const char *use_app = (app && app[0]) ? app : "kdash";
    char value[KDASH_ENDPOINT_MAX];

    /* 2. The current stem. */
    kdash_khlenv_status_t st =
        kdash_khlenv_resolve(use_app, KDASH_CENTRAL_STEM, value, sizeof(value));
    if (st == KDASH_KHLENV_NULL)
        return KDASH_EP_NONE; /* deliberate: do not fall back to anything */
    if (st == KDASH_KHLENV_OK)
        return kdash_parse_hostport(value, KDASH_REDIS_PORT_DEFAULT, host,
                                    hostsz, port)
                   ? KDASH_EP_OK
                   : KDASH_EP_INVALID;

    /* 3. The legacy alias, on a miss only. The new stem is not in the store on
     *    every host yet (CD-3: migrate opportunistically), and both name the
     *    same endpoint. */
    if (st == KDASH_KHLENV_MISS) {
        st = kdash_khlenv_resolve(use_app, KDASH_CENTRAL_STEM_LEGACY, value,
                                  sizeof(value));
        if (st == KDASH_KHLENV_NULL)
            return KDASH_EP_NONE;
        if (st == KDASH_KHLENV_OK)
            return kdash_parse_hostport(value, KDASH_REDIS_PORT_DEFAULT, host,
                                        hostsz, port)
                       ? KDASH_EP_OK
                       : KDASH_EP_INVALID;
    }

    /* 4. khlenv itself is unreachable (or holds nothing anywhere). The central
     *    Redis has lived at the default since kpidash 001; using it cannot be
     *    worse than having no resolver at all. */
    return kdash_parse_hostport(KDASH_CENTRAL_DEFAULT, KDASH_REDIS_PORT_DEFAULT,
                                host, hostsz, port)
               ? KDASH_EP_OK
               : KDASH_EP_INVALID;
}
