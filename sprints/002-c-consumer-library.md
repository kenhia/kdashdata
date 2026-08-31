# Sprint 002 — the shared C consumer library

korg: proposal 1751, WIs 1742/1743/1744. Goal: build `libkdash`, the library
the three LVGL dashboards link to read the feeds sprint 001 documented —
connection and freshness core, typed readers for the five schema'd kpidash
payloads, aarch64 + x86_64 builds with a gate that actually runs on the build
host. No rendering.

## Premise check, before anything was built

Every claim in the three work items was checked against the code and the
machine first. All three held, with two corrections worth having found at
the start rather than at ship time:

- **khlenv has no C client.** The work item said "endpoint resolution through
  khlenv" as if it were a call. Python and Rust clients exist; C does not. The
  protocol is one plain-HTTP GET (`/v1/resolve?app=&key=`, 200/204/404), so
  the library speaks it directly — see CD-9 for why that beat taking libcurl.
- **The dependency budget is two, not one.** The item named hiredis as the
  repo's first third-party dependency. True, but the payloads are JSON, so a
  parser was needed too. Vendored cJSON, as kdeskdash does.

Everything else measured as described: the kdeskdash reference
implementations, the five schemas, the TTLs and windows, the aarch64
toolchain and sysroot.

## What shipped

- **[`include/kdash/`](../include/kdash/)** — the public API, six headers
  behind one umbrella [`kdash.h`](../include/kdash/kdash.h):
  `kdash_keys.h` (key grammar and the token choke point), `kdash_freshness.h`
  (both freshness models plus the CD-6 ladder), `kdash_payload.h` (the five
  payloads), `kdash_endpoint.h` (khlenv + CD-4 discovery), `kdash_conn.h`
  (connection, backoff, reachability), `kdash_feed.h` (the typed readers).
- **[`src/`](../src/)** — the implementation, split pure-core / I/O-shell
  per CD-10. hiredis appears in exactly two translation units and in a
  private header; a dashboard linking this library needs no hiredis headers
  of its own.
- **[`tests/`](../tests/)** — four host unit tests, ~200 assertions, no
  Redis and no network.
- **[`examples/kdash_dump.c`](../examples/kdash_dump.c)** — the toy consumer
  that is WI #1743's acceptance criterion made executable.
- **Build**: [`CMakeLists.txt`](../CMakeLists.txt) with the CMake-config-then-
  pkg-config hiredis lookup, [`cmake/aarch64-toolchain.cmake`](../cmake/aarch64-toolchain.cmake),
  and a [`justfile`](../justfile) where `just check` is now docs + compile +
  unit tests.
- **Decisions**: CD-9 (the dependency budget) and CD-10 (pure core + I/O
  shell, no rendering) in [architecture.md](../docs/architecture.md); CD-4
  gained the library's concrete resolution order and a measurement.

## Decisions made in-sprint

**Freshness is handed to the caller, not applied for them.** The readers
return `ts` and let the panel apply the window. A library that pre-filtered
stale records would take the decision away from the only code that knows how
it wants to render one — a greyed card and a hidden card are different
products. The two models stay visibly different in the API too: the expiring
feeds answer by `KDASH_ABSENT`, the ts-owned ones by arithmetic the caller
does.

**A required field that violates its schema rejects the record whole; a bad
optional costs only itself.** rules.md says required fields are required;
extending that to constraint violations (`cpu_pct: -1` against
`minimum: 0`) is the schema-faithful reading, and it keeps a half-parsed
sample off a panel. Unknown fields are ignored, always — a reader that
rejected them would be the broken half of additive evolution.

**Identity comes from the key, never the payload.** `kpidash:services:*:*`
payloads carry a `host` echo, and it is display-only; the parsers deliberately
do not write the identity fields, so a payload cannot relabel itself. Tested.

**`app`/`key` are validated, not escaped, on the khlenv query.** The token
charset (`[A-Za-z0-9._-]`) reserves nothing in a query string, so refusing
everything else is both less code and a tighter choke point than encoding it.

**A `PING` before the connection is declared usable.** Found while running
the toy consumer with no password: a Redis that accepts the socket but
refuses commands looked *connected*, and every read came back as if the key
were simply absent — so the panel would have said "every host is offline"
instead of "unavailable". One round-trip per connect (i.e. one per backoff
interval at worst) tells those two apart, and they are not close to the same
thing.

**No IPv6 literals in an endpoint.** `kdash_parse_hostport` refuses a second
colon rather than mis-splitting `::1`. The homelab addresses Redis by name or
IPv4; refusing is honest, and silently parsing `fd00::1:6379` as host `fd00`
would not be.

## Verification

`just check` is negative-tested on all four of its arms — each was made to
fail on purpose and watched exit non-zero: a broken markdown link, an
unparseable schema file, a syntax error in the library, and a real logic bug
(allowing `:` into the host-token charset, which took down four assertions
across two test files). A fifth: deleting the `add_test` registrations, to
confirm `--no-tests=error` means a gate that stopped registering tests fails
rather than passing empty.

Cross build confirmed genuine, not merely successful:
`build-aarch64/kdash_dump` is `ELF 64-bit ARM aarch64`, linked
`--sysroot=/home/ken/pi5-sysroot`, `NEEDED libhiredis.so.1.1.0` (the
sysroot's SONAME, not the host's).

Live against the real fleet from kai — the whole discovery chain, end to end:

- khlenv initially 404'd `KDASH_CENTRAL_REDIS` at all three stems — it was
  not in the store — so the miss-then-legacy step fired and `KPIDASH_REDIS`
  answered `rpi53:6379`. Both halves of CD-4's walk exercised for real.
  The stem was then seeded (below), and the new one resolves directly.
- With no password: `endpoint: rpi53:6379 (unreachable)`, every family
  "unavailable", exit 2. Degraded, not wrong, and not hung — CD-6.
- With `REDISCLI_AUTH`: all six client hosts with health/telemetry (`kwork`
  correctly `offline` — its key had expired), both service cards including
  the `_` sentinel rendered as `(no host)`, all three apartment zones.
  0 skipped anywhere.

## Seeded in-sprint: `KDASH_CENTRAL_REDIS`

The sprint's own measurement — that CD-4's new stem was documented but not in
the store — was fixed rather than left as a follow-up. Added to k-homelab's
`khlenv/store.yml` beside the legacy `KPIDASH_REDIS`, both pointing at
`rpi53:6379`, with a comment on each saying they move together. The khlenv
service polls the file's mtime, so it went live with no restart:
`x-khlenv-stem: KDASH_CENTRAL_REDIS`, and `just dump` now resolves through
the new stem instead of the fallback.

## Follow-ups

- **The socket paths have no unit test** — khlenv's HTTP GET and the Redis
  I/O are covered by `just dump` against a live fleet, not by `just check`.
  A loopback stub server would close that, and is worth it only if this code
  starts changing.
- Sprint 003: the Rust and Python publisher wrappers, or the first
  kstudiodash consumption of this library (korg:1728), whichever is wanted
  first.
