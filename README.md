# kdashdata

> **Early days** — contract v0 (docs + schemas), the shared C consumer
> library and the publisher wrappers are here.

kdashdata owns data movement for my homelab's LVGL dashboards: documented
Redis feed contracts (JSON schema files as the machine-readable source of
truth), a shared C consumer library the dashboards link, and thin publisher
wrappers (Rust and Python). The dashboards themselves — kpidash, kdeskdash,
kstudiodash — live in their own repos and consume this one.

## Layout

- [`contracts/rules.md`](contracts/rules.md) — the rules new feeds follow
- [`contracts/registry.md`](contracts/registry.md) — the feed inventory
- [`contracts/schemas/`](contracts/schemas/) — one JSON Schema per feed payload
- [`docs/architecture.md`](docs/architecture.md) — topology and standing decisions
- [`include/kdash/`](include/kdash/) — the C consumer library's public headers
  (start at [`kdash.h`](include/kdash/kdash.h))
- [`src/`](src/) — its implementation; [`tests/`](tests/) — the host unit tests
- [`examples/kdash_dump.c`](examples/kdash_dump.c) — a toy consumer that reads
  every family libkdash has a reader for and prints it
- [`publishers/`](publishers/) — the publisher wrappers: a
  [Rust crate and CLI](publishers/rust/) and a [Python package](publishers/python/)

## The consumer library

`libkdash` is what the LVGL dashboards link to read the feeds — the data
model and freshness rules of the [registry](contracts/registry.md) plus a
Redis client that degrades instead of blocking. **No rendering:** it has no
knowledge of LVGL and never will.

```c
#include <kdash/kdash.h>

kdash_conn_t *c = kdash_conn_new(&(kdash_conn_opts_t){.app = "kstudiodash"});

char hosts[16][KDASH_TOKEN_MAX];
int n = kdash_clients(c, hosts, 16, NULL);
for (int i = 0; i < n; i++) {
    kdash_telemetry_t t;
    if (kdash_telemetry(c, hosts[i], &t) == KDASH_OK)
        render(&t);   /* fresh by construction — the key carries a TTL */
}
```

The `claude:*` family lives at its **own** khlenv stem (CD-7), so it gets its
own handle — and its readers hand back a derived display state and an
attention-first order, not just parsed rows (CD-16):

```c
kdash_conn_t *cc = kdash_conn_new(&(kdash_conn_opts_t){
    .app = "kstudiodash", .stem = &KDASH_STEM_CLAUDE});

kdash_claude_session_t s[16];
int n = kdash_claude_sessions(cc, s, 16, NULL);
kdash_claude_sessions_refresh(s, n, time(NULL), KDASH_CLAUDE_IDLE_S,
                              KDASH_CLAUDE_STALE_S);
/* s[0] is whatever most wants your attention; s[i].disp says why */
```

A dashboard can also be **told** something — three control feeds, still
read-only, because acting on a command is a matter of noticing its `ts` advance
rather than consuming it (CD-17). One family per verb, each with its own edge
(CD-22):

```c
kdash_panel_t p;
if (kdash_panel(c, my_hostname, &p) == KDASH_OK &&
    kdash_panel_actionable(&p, last_acted_ts, time(NULL), KDASH_PANEL_WINDOW_S))
    show(p.want);   /* KDASH_PANEL_DASH or KDASH_PANEL_DESKTOP */

kdash_panelmode_t m;   /* which screen within the dashboard, and how to set it up */
if (kdash_panelmode(c, my_hostname, &m) == KDASH_OK &&
    kdash_cmd_actionable(m.ts, last_acted_mode_ts, time(NULL),
                         KDASH_PANEL_WINDOW_S)) {
    const char *density = kdash_setting_get(&m, "density"); /* NULL = leave it */
    switch_to(m.mode, density);
}
```

`kdash_panelshot()` is the third, and `mode` is deliberately not a closed enum:
the screen names are the dashboard's vocabulary, so the contract validates the
shape and the dashboard ignores a mode it does not have.

The endpoint comes from khlenv (CD-4) and the password from `REDISCLI_AUTH`
(CD-2), so a consumer hardcodes neither. Builds for x86_64 natively and for
aarch64 with `just build-aarch64`. Dependencies are hiredis (system) and a
vendored cJSON, and no more — CD-9.

**Consuming it as a submodule.** `add_subdirectory()` on this repo gives you
the `kdash` target and nothing else: `kdash_dump` and the unit tests build only
when this is the top-level project (`KDASH_BUILD_EXAMPLES` /
`KDASH_BUILD_TESTS`), so your `ctest` stays yours. Pass
`-DKDASH_HIREDIS_STATIC=ON` for a binary that needs no `libhiredis` on the host
it is copied to.

## The publisher wrappers

`publishers/` is the write side: find the Redis through khlenv, authenticate,
check the key against the grammar, stamp `ts`, pick a publish pattern. Two
implementations because the publishers are two shapes — a native
[CLI](publishers/rust/) for shell publishers on a hot path (Claude Code hooks:
18 ms per publish) and a [Python package](publishers/python/) for daemons
(102 ms, amortised over a process lifetime). CD-11 has the reasoning.

```sh
kdash-pub setex kdash:selftest:kai 300 '{"host":"kai","publisher":"rust"}'
just pub-endpoint    # where would this host publish?
just pub-check       # ...and would the write be accepted? (sprint 016, CD-25)
```

Those two are deliberately separate. `endpoint` opens a socket and issues no
command, and Redis only checks AUTH when AUTH is *sent* — so `--no-auth`
against the authenticated central Redis exits **0** for a configuration that
cannot write a single key. `check` round-trips a `PING`, so exit 0 means the
server accepted an authenticated command. CD-25 has the measurements.

Plus the point reads a publisher needs to write *correctly* — `hget`, and
(sprint 015) `get` and `scan`, the latter two in both wrappers. Nothing here
consumes a feed; that is `libkdash`. On `get`, **absence is its own answer**:
exit 0 with the value, 1 when the key is not set, 2 when the question could not
be asked at all — because `kdash:stale` is presence-owned and reading an
unreachable Redis as "absent" would restamp a `since` that must be carried
forward unchanged. CD-14 has the reasoning.

```python
from kdash_pub import Publisher
Publisher("apt-temps").publish_latest("kpidash:apttemps:office", {"temp_c": 22.4})
```

## Development

Uses the [kprojects](https://github.com/kenhia/kprojects) minimal harness:
`just` lists recipes, `just check` runs the gates.

`just check` is four gates, in increasing order of what they need installed:

- `check-docs` — python3 only: every JSON file parses, every relative markdown
  link resolves, every schema is listed in the registry **and validates its own
  `examples` and `x-counterexamples`** (CD-24 — a narrow stdlib validator,
  `scripts/jsonschema_mini.py`, which refuses any keyword it cannot check
  rather than skipping it), the registry's families match both publisher
  allowlists in both directions, every `.ps1` is pure ASCII (Windows PowerShell
  5.1 reads a BOM-less script as the ANSI codepage), and `CLAUDE.md`'s Status
  line names the newest `sprints/` record.
- `check-python` — python3 only: the Python wrapper's pure core, which imports
  nothing but the stdlib precisely so this gate needs nothing installed.
- `check-rust` — cargo: fmt, clippy, and the Rust wrapper's unit tests. A first
  build also needs network and git access to the private khlenv repo (CD-11).
- the C library build plus ctest — the pure core (key grammar, freshness,
  payload parsing) with no Redis and no network.

None of the four opens a socket — with one named narrowing: `test_feed` swaps
`kdash_feed.c`'s Redis calls for a fake so the counted readers' `-1` contract
is exercised rather than asserted (sprint 016, CD-10 as amended). The rest of
the socket code is verified live: `just dump` reads the real central Redis
(needs `REDISCLI_AUTH`), and `just pub-check` plus the publisher self-test
prove the write path.

Building the C library needs `libhiredis-dev`; the aarch64 cross build
additionally needs `gcc-aarch64-linux-gnu` and a Pi sysroot at `~/pi-sysroot`
(kdeskdash's `just sync-sysroot` populates one, and the same sysroot serves
both repos).

## License

MIT — see [LICENSE](LICENSE).
