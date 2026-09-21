# Publisher wrappers

The **write** side of the [feed contracts](../contracts/rules.md). The read
side is `libkdash` ([`include/kdash/`](../include/kdash/)); nothing here
renders, and nothing here *consumes* — what both wrappers have is the handful
of point reads a publisher needs to write CORRECTLY, and that is the whole of
it (CD-14, amended in sprint 015).

A publisher's whole job is small and nobody gets all of it right:

| Obligation | Decision |
|---|---|
| find the Redis, and notice when it moves | CD-4 — khlenv, re-resolved on every connect |
| authenticate | CD-2 / CD-12 / CD-19 — `REDISCLI_AUTH`, or an env file where that cannot reach: the per-host `/etc/khomelab/secrets.env` (`%ProgramData%\khomelab\secrets.env` on Windows) first, the per-user files after it and deprecated |
| write a key a reader will parse | [rules.md](../contracts/rules.md) key grammar, enforced at the publish choke point |
| stamp `ts` | [rules.md](../contracts/rules.md) payload rules |
| pick a publish pattern | latest-value expiring / ts-owned / capped event log |

These two packages are that derivation, once each.

## The three reads, and why a publisher has any

A publisher reads for one reason: it cannot write correctly otherwise. Nothing
here decodes a record, runs a freshness ladder or has a notion of a feed — the
verbs return bytes and key names.

| verb | question | where |
|---|---|---|
| `hget <key> <field>` | is somebody's observation fresher than mine? (`claude:limits`) | Rust CLI |
| `get <key>` | what did the last writer put here, so I can carry it forward unchanged? (`kdash:stale`'s `since`) | Rust CLI, Python `Publisher.get` |
| `scan <pattern>` | which keys exist? — the identities are IN the key for `kdash:stale:{host}:{deployer}` | Rust CLI, Python `Publisher.scan` |

**Absence is an answer, and on `get` it is a distinct one.** `hget` folds
"absent field" and "absent key" together, because Redis does and the guard it
serves does not care. `get` cannot: `kdash:stale` is presence-owned (CD-18),
where absence is the *only* all-clear, so a caller that cannot tell "not set"
from "set to empty" cannot implement the feed. The CLI says so in its exit
code:

```sh
kdash-pub get kdash:stale:komarchy:claude-hooks
#   0  present — the value, plus a newline, on stdout
#   1  absent  — nothing on stdout
#   2  could not ask — unreachable, auth failed, or the key was refused

kdash-pub scan 'kdash:stale:komarchy:*'
#   0  answered — one key per line, and NO lines is a complete answer
#   2  could not ask
```

The three outcomes are `just published`'s on purpose, so one idiom covers
both. And **`--best-effort` does not touch these two**: it exists so a dead
Redis cannot fail a hook, and folding 2 into 0 here would spell "I could not
ask" exactly like "here is the answer". A caller that read an unreachable
Redis as "absent" would restamp `since` and turn "stale for three weeks" into
"stale for an hour", while looking like working code.

```sh
if since=$(kdash-pub get "kdash:stale:$host:$deployer"); then
    :                                   # carry it forward unchanged
elif [ $? -eq 1 ]; then
    since=$(date +%s)                   # genuinely the first skip
else
    exit 2                              # could not ask — do NOT guess
fi
```

**A pattern is not a key.** `scan` takes `*` and `?` inside a segment and
nothing else — no `[abc]` classes, no escapes — and **the namespace may not be
globbed**, so `*:*` is refused. A publisher's read is a read of its own feed,
on a Redis it shares with kvscf and the dashboards. `scan` uses SCAN rather
than KEYS, which would block the server for the whole sweep.

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
just deploy-komarchy  # the laptop — knarr, but only with the lid open
just deploy-all     # all four publisher hosts, which is the point
```

`publish` refuses a dirty tree and refuses a stamp that names no commit, and
off `main` it publishes without moving `latest`. The install paths are a
contract: the CD-7 hook scripts exec them absolutely, because a hook context's
`PATH` is not the interactive one.

Verify a rollout by naming the hosts — `kdash-pub --version` on kai, kubs0,
cleo **and** komarchy — never by iterating the hosts the runner happened to
reach. `kdash-pub --app kdashdata endpoint` is the stronger per-host check: it
proves khlenv resolution and the CD-12 auth route work on that host, not just
that a file landed. On a host with a systemd user manager, run it *through*
one — `systemd-run --user --collect --wait --pipe kdash-pub --app kdashdata
endpoint` — because a login shell's groups and a user manager's groups are
different facts, and the per-host secrets file is readable by group.

komarchy is the host this rule is easiest to lose: it is asleep most of the
time, so it is the one that silently keeps an old binary. It ran
`0.1.0-7fe2c87` for ten days after the rest of the fleet moved on, which is
how CD-19 came to be shipped everywhere except the laptop.

## Gates

```sh
just check-python   # stdlib only — runs with neither redis nor khlenv installed
just check-rust     # fmt, clippy, unit tests
just pub-endpoint   # live: where would this host publish, and can it?
just pub get kdash:selftest:$(hostname -s)    # live: the read path, end to end
```

`check-python` needs nothing installed because the pure core (`keys`,
`payload`, `auth`, `endpoint.resolve_with`) imports nothing but the stdlib —
CD-10's split, applied to the publish side. `check-rust` needs cargo and, on a
first build, git access to the private khlenv repo (CD-11).

The socket halves are not unit-tested. They are verified live, from a real
host, against the real fleet — `just pub-endpoint` and the
[self-test](python/examples/selftest.py), which publishes
[`kdash:selftest:<host>`](../contracts/schemas/kdash-selftest.schema.json).
