# Sprint 003 — publisher wrappers + khlenv stems

korg: proposal 1752, WIs 1745/1746/1747. Slice 1 of the claude-feed relocation
program (korg:1755, CD-7). Goal: the publish side of the contracts — a Rust
crate and fast-startup CLI, a Python wheel, and the khlenv stems and auth
conventions that turn the eventual `claude:*` cutover into a store edit instead
of a sweep of every publisher host.

## Premise check, before anything was built

Two of the three work items had drifted, and both cost about a minute to find.

- **#1745 — "only a Python khlenv client exists today".** Not true any more:
  khlenv sprint 003 shipped a Rust one (`clients/rust`, tag
  `khlenv-client-v0.1.0`, `git ls-remote` resolves it). The item's contingency —
  file a khlenv slice, or speak khlenv's HTTP directly — was moot. Drift in the
  helpful direction; the sprint took the client (CD-11).
- **#1746 — "lift kpidash-client's `endpoint.py`".** Held exactly: re-resolve on
  every connect, config override wins outright, khlenv null means deliberately
  no endpoint, password only from `REDISCLI_AUTH`.
- **#1747 — "put `KDASH_CENTRAL_REDIS` in the khlenv store".** Already done, by
  sprint 002, which seeded it and recorded so in CD-4. The item shrank to its
  other half — the claude stem and the hook-context auth convention — and that
  half turned out to be bigger than written (below).

No cross-project plan applies (kdashdata is not in
`cross-project-planning/index.md`).

## The measurement that shaped the sprint

WI #1747 asked to *document* how `REDISCLI_AUTH` reaches non-daemon contexts.
Measured on kai, there is nothing to document — **it does not reach them at
all**:

| context | route | `REDISCLI_AUTH` |
|---|---|---|
| kpidash-client daemon | `EnvironmentFile=~/.config/kpidash-client/redis-auth.env` (0600) | set |
| interactive fish, started by hand | `~/.kconfidential/api_tokens.fish`, inside `status is-interactive` | set |
| Claude Code process, and `bash -lc` | — | **UNSET** |
| a Claude Code hook | inherits the above | **UNSET** |

`~/.claude/settings.json` has no `env` block, and a password in one would be
worse than the gap. `claude-pub.sh` works today only because `rpidash2:6380` is
unauthenticated — which is precisely why the CD-7 repoint could not have gone
first. Repointing at central without solving this would not have degraded; it
would have silently published nothing.

That turned #1747 from a documentation task into CD-12.

## What shipped

- **[`publishers/rust/`](../publishers/rust/)** — `kdash-pub`, a crate and a
  CLI. Seven verbs, a `--stem`/`--endpoint`/`--no-auth`/`--best-effort` front
  end, and a tab-separated `batch` mode that puts a whole hook's worth of
  writes through one connection.
- **[`publishers/python/`](../publishers/python/)** — `kdash_pub`, wheel-built,
  `khlenv` + `redis` from the homelab package store. Three publish patterns:
  latest-value expiring, latest-value ts-owned, capped event log.
- **[`publishers/README.md`](../publishers/README.md)** — which wrapper, and
  why there are two.
- **The `KDASH_CLAUDE_REDIS` stem**, seeded live in k-homelab's
  `khlenv/store.yml` at `rpidash2:6380`, with a comment aimed at whoever
  performs the flip (including CD-8's kvscf pin).
- **A first `kdash:*` feed** —
  [`kdash:selftest:{host}`](../contracts/schemas/kdash-selftest.schema.json),
  registered, with the Python example that writes it. The namespace stops being
  reserved-and-empty.
- **Decisions**: CD-11 (the publish-side dependency budget, the two-language
  split, and why both take the khlenv client) and CD-12 (`REDISCLI_AUTH` in
  hook contexts) in [architecture.md](../docs/architecture.md). CD-4 gained the
  claude stem and the reader-vs-writer fallback rule; CD-7 gained what this
  slice bought and what it measured.
- **Contracts**: a *Publisher conduct* section in
  [rules.md](../contracts/rules.md); a stem-aware Homes table plus the `kdash`
  family in [registry.md](../contracts/registry.md).
- **Gates**: `just check` grew from two to four —
  `check-docs`, `check-python`, `check-rust`, then the C build and ctest.

## Decisions made in-sprint

**Take the khlenv Rust client; do not re-derive it.** The C library speaks
khlenv's HTTP directly because there is no C client and libcurl was the worse
trade (CD-9). Rust has a client, and its endpoint precedence — the file,
`$KHLENV_ENDPOINT`, HKCU then HKLM on Windows — is a *cross-client contract*
the Python and Rust clients pin together. A third implementation would be a
third answer to "where is khlenv", which is the one question that must have
one. The price is a git dependency on a private repo, so a first `cargo build`
needs network and credentials; `check-docs` and `check-python` stay free of
that.

**Two languages, split by latency, with the numbers to say so.** Measured on
kai: a full publish costs **18 ms** through `kdash-pub` and **102 ms** through
the Python package; startup alone is 0.4 ms against 64 ms of import. A daemon
amortises an import over its process lifetime and a hook firing on every tool
call does not — which is the whole reason the CD-7 cutover vehicle is an exec.

**A compiled-in endpoint default is a reader's privilege, not a writer's.**
`Stem::CENTRAL` keeps CD-4's `rpi53:6379` fallback; `Stem::CLAUDE` deliberately
has none. For a dashboard, a stale-but-plausible endpoint still renders and
nothing does not. For a publisher the arithmetic inverts: a misdirected write
is a convincing record in the Redis nobody is reading, and it looks like
success from both ends. The stem whose *value is about to change* is exactly
the one that must not be guessed — so a publisher that cannot resolve it drops
the write and says so.

**"I have a password" and "this server wants one" are different questions.**
Found by running the CLI against the interim claude home: sending AUTH to a
Redis with none configured is an error
(`ERR AUTH <password> called without any password configured`), not a no-op.
A publisher that finds a password and always sends it therefore *cannot write
to `rpidash2:6380` at all* — which would have made one publisher unable to
serve the dual-write window. Hence the explicit `--no-auth` / `without_auth()`
/ `authenticate=False`. Explicit rather than a retry-without-password
fallback: a silent retry would turn a wrong password into a connection that
succeeds and then fails NOAUTH on every command, which is worse than the
failure it hid.

**Exit codes split contract errors from delivery errors, and `--best-effort`
only forgives the second.** A dead Redis must never fail a hook — that is
CD-6 — so delivery failure becomes exit 0 on request. A key that violates the
grammar stays exit 1 either way: quietly exiting 0 on an off-contract key is
how a publisher goes off-contract for a month without anyone finding out.

**Batch mode is tab-separated, and that is a decision.** JSON strings cannot
contain a literal tab, so the format needs no quoting rules — and a format
with no escaping has no escaping bugs. It exists because `claude-pub.sh`
pipelines several commands per hook; a CLI that could only do one write per
exec would be *slower* than the bash it replaced.

**The pure-core/I-O-shell split (CD-10) earns more here than it does for the
reader.** In Python it is what lets `just check-python` run with neither
`redis` nor `khlenv` installed: `keys`, `payload`, `auth` and
`endpoint.resolve_with` import nothing but the stdlib, and `__init__` reaches
`Publisher` through a PEP 562 `__getattr__` so importing the package does not
drag `redis` in.

**A new gate: every schema is linked from the registry.** rules.md says a feed
exists when its schema file lands — so a schema the registry never mentions is
a feed nobody can find, the same class of fault as a broken link one level up.
It caught its first real drift immediately: this sprint's own
`kdash-selftest.schema.json`.

## Verified live, from kai

- `kdash-pub endpoint` → `rpi53:6379`, resolved through khlenv and connected,
  with **`REDISCLI_AUTH` unset in the shell** — so the CD-12 file route
  (`~/.config/kpidash-client/redis-auth.env`) is what authenticated it.
- A wrong password fails: `REDISCLI_AUTH=definitely-not-the-password` → exit 2,
  `Password authentication failed`. AUTH is genuinely happening, not skipped.
- `kdash:selftest:kai` published by **both** wrappers and read back off
  `rpi53:6379` with a correct `ts` and TTL 300.
- `batch` mode: an `hset` + `expire` pair over one connection, read back with
  the auto-stamped `ts` and a 60 s TTL.
- The new stem answers live — `x-khlenv-stem: KDASH_CLAUDE_REDIS` →
  `rpidash2:6380` — and a `--no-auth` write landed there and read back. The
  same command without `--no-auth` fails, exit 2, as designed.
- `weather:now` is refused with exit 1 even under `--best-effort`; an
  unreachable endpoint is exit 2, or exit 0 with it.

## Gates, negative-tested

Each was made to fail on a planted fault before being trusted:

- `check-docs` — an unregistered schema, malformed JSON, and a broken markdown
  link: exit 1 on each.
- `check-python` — the namespace check stubbed to `if False`: 2 failures,
  exit 1.
- `check-rust` — the same hole on the Rust side: 2 failures, exit 101.

## Follow-ups

- **The Python wheel is built, not published.** `just pub-wheel` produces it;
  pushing it to the homelab package store is a deliberate separate step, and
  nothing consumes it yet.
- **`kvscf:*` has no khlenv stem.** CD-8 requires its endpoint to be pinned
  explicitly when `claude:*` repoints, because kdeskdash defaults one to the
  other. Naming that stem belongs to the kdeskdash slice that performs the
  flip (korg:1753); the store comment says so.
- **Neither socket path is unit-tested** — same shape as sprint 002's
  follow-up, same answer: live verification, now `just pub-endpoint` and the
  self-test alongside `just dump`.
- **Next in the program**: korg:1753, the kdeskdash publisher cutover and
  reader repoint — `claude-pub.sh` becomes `kdash-pub` calls, dual-writing
  through the window, with `--no-auth` on the interim side only.
