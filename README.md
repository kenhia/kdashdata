# kdashdata

> **Early days** — contract v0 (docs + schemas) and the shared C consumer
> library are here; the publisher wrappers are not yet.

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
  every schema'd family and prints it
- the Rust/Python publisher wrappers arrive in a later sprint; their
  directories are created when they carry code, not before

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

The endpoint comes from khlenv (CD-4) and the password from `REDISCLI_AUTH`
(CD-2), so a consumer hardcodes neither. Builds for x86_64 natively and for
aarch64 with `just build-aarch64`. Dependencies are hiredis (system) and a
vendored cJSON, and no more — CD-9.

## Development

Uses the [kprojects](https://github.com/kenhia/kprojects) minimal harness:
`just` lists recipes, `just check` runs the gates.

`just check` is two gates. `check-docs` needs only python3 — every JSON file
parses, every relative markdown link resolves — so it runs on a host with no
compiler. The rest builds the library and runs the unit tests, which cover the
pure core (key grammar, freshness, payload parsing) with no Redis and no
network. The socket code is verified live instead: `just dump` reads the real
central Redis, and needs `REDISCLI_AUTH` set.

Building needs `libhiredis-dev`; the aarch64 cross build additionally needs
`gcc-aarch64-linux-gnu` and a Pi sysroot at `~/pi-sysroot` (kdeskdash's
`just sync-sysroot` populates one, and the same sysroot serves both repos).

## License

MIT — see [LICENSE](LICENSE).
