# Publisher wrappers

The **write** side of the [feed contracts](../contracts/rules.md). The read
side is `libkdash` ([`include/kdash/`](../include/kdash/)); nothing here
renders, and nothing here *consumes* — the Rust CLI has one point read
(`hget`) for a publisher guarding its own write against a fresher observation,
and that is the whole of it (CD-14).

A publisher's whole job is small and nobody gets all of it right:

| Obligation | Decision |
|---|---|
| find the Redis, and notice when it moves | CD-4 — khlenv, re-resolved on every connect |
| authenticate | CD-2 / CD-12 — `REDISCLI_AUTH`, or a 0600 env file where that cannot reach |
| write a key a reader will parse | [rules.md](../contracts/rules.md) key grammar, enforced at the publish choke point |
| stamp `ts` | [rules.md](../contracts/rules.md) payload rules |
| pick a publish pattern | latest-value expiring / ts-owned / capped event log |

These two packages are that derivation, once each.

## Which one

| | [Rust](rust/) — `kdash-pub` | [Python](python/) — `kdash_pub` |
|---|---|---|
| For | shell publishers on a hot path: Claude Code hooks, statuslines | daemon publishers: kpidash-client and friends |
| Shape | a crate **and** a CLI binary | a wheel from the homelab package store |
| Full publish, measured on kai | **18 ms** | **102 ms** |
| Startup alone | 0.4 ms | 64 ms of import |

The split is by latency, not taste — see CD-11. A daemon amortises an import
over its process lifetime; a hook that fires on every tool call does not, and
that is why the CD-7 cutover vehicle for `claude-pub.sh` is an exec of a native
binary rather than a Python import.

Both speak the same contract, resolve through the same khlenv stems, read the
same env files, and refuse the same keys. Where they disagree, one of them is
wrong — which is why the two test suites pin the same tables of accepted and
refused inputs, and why those tables also match `kdash_keys.h` and
`kdash_endpoint.c` on the reader side.

## Distribution

`kdash_pub` (Python) is a wheel; `kdash-pub` (Rust) is a binary the hook hosts
exec by absolute path, so it ships as a package-store artifact — CD-13.

```sh
just version        # the store label this checkout would publish under
just publish        # linux + windows cross-build, ONE version, from main
just deploy         # knarr -> /usr/local/bin/kdash-pub on kai and kubs0
just deploy-cleo    # store-resolving install -> C:\tools\bin\kdash-pub.exe
just deploy-all     # all three publisher hosts, which is the point
```

`publish` refuses a dirty tree and refuses a stamp that names no commit, and
off `main` it publishes without moving `latest`. The install paths are a
contract: the CD-7 hook scripts exec them absolutely, because a hook context's
`PATH` is not the interactive one.

Verify a rollout by naming the hosts — `kdash-pub --version` on kai, kubs0
**and** cleo — never by iterating the hosts the runner happened to reach.
`kdash-pub --app kdashdata endpoint` is the stronger per-host check: it proves
khlenv resolution and the CD-12 auth route work on that host, not just that a
file landed.

## Gates

```sh
just check-python   # stdlib only — runs with neither redis nor khlenv installed
just check-rust     # fmt, clippy, unit tests
just pub-endpoint   # live: where would this host publish, and can it?
```

`check-python` needs nothing installed because the pure core (`keys`,
`payload`, `auth`, `endpoint.resolve_with`) imports nothing but the stdlib —
CD-10's split, applied to the publish side. `check-rust` needs cargo and, on a
first build, git access to the private khlenv repo (CD-11).

The socket halves are not unit-tested. They are verified live, from a real
host, against the real fleet — `just pub-endpoint` and the
[self-test](python/examples/selftest.py), which publishes
[`kdash:selftest:<host>`](../contracts/schemas/kdash-selftest.schema.json).
