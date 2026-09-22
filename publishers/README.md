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

## The two probes, and the one that does not prove what it looks like

`endpoint` and `check` ask the same question one clause apart, and the gap
between them is the whole reason there are two (CD-25):

| verb | question | what exit 0 means |
|---|---|---|
| `endpoint` | **where** would this host write? | resolved, and a socket opened |
| `check` | **would the write be accepted?** | ...and a command came back |

`endpoint` issues **no command**, and Redis only checks AUTH when AUTH is
*sent*. So a *wrong* password fails at connect — but `--no-auth` against a
server that requires one connects happily and fails `NOAUTH` on the first real
command. Measured on kubs0 2026-09-12 and again on kai 2026-09-21 (WI 2492),
against the authenticated `rpi53:6379`:

```sh
kdash-pub --app kdashdata endpoint              # 0  — correct
kdash-pub --app kdashdata --no-auth endpoint    # 0  — and it cannot write a single key
kdash-pub --app kdashdata --no-auth check       # 2  — NOAUTH: Authentication required
```

That is not a bug in `endpoint`: "where would this write" is a fair question
with a fair answer, and it is the cheap one. But it is a **sharp edge**, and a
caller using `endpoint` to mean "this host can publish" needs a paired
negative control to mean anything by it. `check` is that question asked
directly:

```sh
kdash-pub check
#   0  accepted — resolved, connected, and a PING came back
#   1  deliberately nowhere — khlenv holds an explicit null for this stem
#   2  could not ask — unreachable, auth failed, or the command was refused
```

Exit 1 is `get`'s shape: a clean negative that is not a fault. A host khlenv
says publishes nowhere is *correctly configured*, and reporting that as an
unreachable Redis would be the same conflation the `get` table exists to
prevent — which is also why **`--best-effort` does not touch `check`** either.

What exit 0 proves is narrow on purpose: the endpoint resolved, the socket
opened, and the server accepted an **authenticated** command. It does not
prove this connection may *write* — that would need a write, and a probe that
writes is not a probe.

**The Python wrapper has no equivalent and needs none**, for a reason worth
knowing: `Publisher.connect()` is lazier still — redis-py builds a pool and
opens no socket — but it was never sold as a probe. Every path that reaches
Redis (`get`, `scan`, `publish_*`) issues a real command and authenticates for
real, so there is no false claim to fix.

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
just published <v>  # is <v> already in the store? 0 = yes, 1 = no, 2 = unknown
just publish        # linux + windows cross-build, ONE version, from main
just publish --dry-run
just deploy         # knarr -> /usr/local/bin/kdash-pub on kai and kubs0
just deploy-cleo    # store-resolving install -> C:\tools\bin\kdash-pub.exe
just deploy-komarchy  # the laptop — knarr, but only with the lid open
just deploy-all     # all four publisher hosts, which is the point
```

`publish` refuses a dirty tree and refuses a stamp that names no commit, and
off `main` it publishes without moving `latest`. The install paths are a
contract: the CD-7 hook scripts exec them absolutely, because a hook context's
`PATH` is not the interactive one.

### The version, and why it skips

The label is `<crate version>-<short sha of the last commit touching the
binary's inputs>`, where the inputs are `publishers/rust/src`, `build.rs`,
`Cargo.toml` and `Cargo.lock` — **not `HEAD`**. The build is a release
`cargo build` of a fixed source tree, so a commit touching only `docs/`,
`contracts/` or `sprints/` produces a byte-identical binary; stamping it with
`HEAD` would publish that binary under a new label and churn the store's
`latest` and every fleet install for no change. Most kdashdata sprints are
contract-only — sprint 012 was schema and docs with no publisher change — and
[`.sprint-deploy`](../.sprint-deploy) now runs `publish` on every ship, so this
is the rule that makes the declaration safe. klaude-top and kdeskdash derive
theirs the same way, by Ken's decision of 2026-09-17, so the three recipes read
alike.

`publishers/python/**` is deliberately **not** an input: it is a separate wheel
with a separate publish step, and changing it leaves this binary identical.

**Changed only build flags? Bump the crate version.** The input set names
source, not the recipe that builds it — so editing the `cargo build` line in
the `justfile` changes the binary without moving the stamp, and `publish` would
then correctly skip a build that genuinely differs. `publishers/rust/Cargo.toml`
is in the input set precisely so this has a one-line remedy.

`just publish` asks the store whether that version already exists and, if it
does, prints `nothing to publish: <version> already in the store` and exits
**0**. A sprint that changed no inputs runs it and it does nothing, loudly.

`just published` **refuses to guess.** A downed store host, a missing host key
and a genuinely absent version all make `ssh … test -d` exit non-zero, and
reading any of those as "absent" would republish over a store nobody could see
— a false claim about the world rather than a failed command. So the remote
answers with a *word*, and an unreachable store is exit 2, which `publish`
treats as a refusal rather than a green light.

The store label and the binary's own `--version` stamp are **one fact**:
`just version` reassembles it from git (so the predicate needs no
cross-compile), `build.rs` derives it again inside the binary, and `publish`
compares the two and refuses if they disagree. That is what keeps the two
input lists from drifting apart silently.

Verify a rollout by naming the hosts — `kdash-pub --version` on kai, kubs0,
cleo **and** komarchy — never by iterating the hosts the runner happened to
reach. `kdash-pub --app kdashdata check` is the stronger per-host check: it
proves khlenv resolution and the CD-12 auth route work on that host, not just
that a file landed. Use `check` and not `endpoint` here, and the reason is the
section above — `endpoint` issues no command, so it cannot tell a working auth
route from no credential at all. On a host with a systemd user manager, run it
*through* one — `systemd-run --user --collect --wait --pipe kdash-pub --app
kdashdata check` — because a login shell's groups and a user manager's groups
are different facts, and the per-host secrets file is readable by group.

komarchy is the host this rule is easiest to lose: it is asleep most of the
time, so it is the one that silently keeps an old binary. It ran
`0.1.0-7fe2c87` for ten days after the rest of the fleet moved on, which is
how CD-19 came to be shipped everywhere except the laptop.

## Gates

```sh
just check-python   # stdlib only — runs with neither redis nor khlenv installed
just check-rust     # fmt, clippy, unit tests
just pub-endpoint   # live: where would this host publish?
just pub-check      # live: ...and would the write be accepted? (CD-25)
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
