# kdashdata architecture

> Contract v0 — sprint 001. Decisions carry `CD-n` ids; open questions carry
> `OQ-n` ids. Cite them from sprint records and sibling repos.

## The shape of the system

Three LVGL dashboards consume homelab data feeds over Redis:

| Dashboard | Runs on | Arch | Role |
|---|---|---|---|
| kpidash | rpi53 (Pi 5) | aarch64 | Fleet KPI wall — also hosts the central Redis |
| kdeskdash | rpidash2 (Pi 5, dev desk), rpidash3 (Pi 4, work desk) | aarch64 | Multi-mode desk panels |
| kstudiodash | kstudio (Surface Studio) | x86_64 | Big-panel dashboard |

The rpidash3 (work-desk) kdeskdash instance participates in the shared feeds
but is otherwise out of kdashdata's scope.

Publishers (kpidash-client daemons, Claude Code hooks, kvscf, apt-temps…)
live in their own repos. kdashdata owns the **contracts** they write against,
the **shared C consumer library** the dashboards link (sprint 002+), and the
thin **Rust/Python publisher wrappers** they write through (sprint 003+).
Nothing in this repo runs as a service.

## CD-1 — Topology: one central Redis + per-dashboard local Redis

- **Shared / cross-dashboard data** lives on the **central Redis**:
  `rpi53:6379`, AUTH required. It has been the de facto hub since kpidash
  001; this makes it policy.
- **Dashboard-specific state** (mode persistence, game scores, device
  control) lives on a Redis **local to that dashboard's host**
  (`127.0.0.1:6379`), in that dashboard's own namespace.
- **No distributed Redis, no replication, no cluster.** A homelab does not
  have that problem; the failure mode is "central Redis unreachable", and the
  answer is CD-6 (degrade, don't block), not more Redis.

One exception remains, by design: the kvscf family stays off-central (CD-8).
The Claude-activity family was the other and no longer is — it completed its
move to the central Redis in sprint 005 (CD-7).

## CD-2 — Auth: one fleet password, `REDISCLI_AUTH` is the contract

A single shared password covers every Redis in the dashboard fleet,
registered in **krot** (`rpi53-redis-password`; krot #1293 tracks the
consumer list). Processes receive it via the **`REDISCLI_AUTH`** environment
variable — that is the contract; a client that takes the password any other
way is divergent (known case: the Windows kpidash-client reads an inline
config field — kpidashclient-win #1294).

Secrets never go through khlenv (CD-4) and never appear in payloads, keys,
or this repo.

OQ-2 tracks a possible Redis ACL split (write-capable `publisher` /
read-only `dashboard` users — two krot entries, least privilege without
password sprawl). Until then: one password, `default` user.

## CD-3 — No big-bang renames

Contract v0 documents today's keys **as they are** (`kpidash:client:*` and
friends stay). New shared feeds land under the `kdash:` namespace
([rules](../contracts/rules.md)); grandfathered families migrate
opportunistically, one at a time, each migration recorded in the
[registry](../contracts/registry.md). A reader must never break because a
writer was upgraded first, or vice versa.

## CD-4 — Endpoint discovery through khlenv

khlenv is the homelab's config resolver (`KEY.<host>.<app>` → `KEY.<host>`
→ `KEY`); kpidash-client already resolves its endpoint through it on every
connect, with `[redis] host` in local config as an explicit override and an
explicit khlenv null meaning "deliberately no endpoint".

kdashdata adopts that model fleet-wide:

- **`KDASH_CENTRAL_REDIS`** — `host:port` of the central Redis. New stem;
  all new consumers and publishers use it.
- **`KPIDASH_REDIS`** — legacy alias for the same endpoint (app
  `kpidash-client`); stays until its publishers migrate (CD-3).
- **`KDASH_CLAUDE_REDIS`** — `host:port` of the `claude:*` family's home
  (sprint 003). Answers `rpi53:6379` since the CD-7 cutover. That flip was the
  whole reason the stem exists, and it stays a *distinct* stem afterwards
  rather than collapsing into `KDASH_CENTRAL_REDIS`: the family keeps its own
  address, so it can move again without a publisher host being touched.
- **Local Redis needs no discovery** — it is `127.0.0.1:6379` by definition;
  per-app env overrides (e.g. `KDESKDASH_REDIS_HOST`) stay as-is.
- Feed families not on the central Redis keep their per-app env
  configuration. `kvscf:*` is the only one left, and CD-8 settles it
  permanently rather than pending anything.

Resolution rules inherited from the kpidash-client implementation: resolve
on **every** connect (a moved Redis is picked up within one loop interval);
local config override wins outright; khlenv never holds the password (CD-2).

The library's resolution order, as implemented in sprint 002: explicit
`$KDASH_CENTRAL_REDIS` in the environment, then khlenv's new stem, then —
**on a 404 miss only** — khlenv's legacy alias, then the historical default
`rpi53:6379` when khlenv itself cannot be reached. A `204` at either stem is
an explicit null and stops the walk: "deliberately no endpoint" is an answer,
not a reason to fall back further.

Both central stems are seeded in the khlenv store (k-homelab
`khlenv/store.yml`) and resolve to `rpi53:6379` as of 2026-08-31 — the new one
added in sprint 002, after that sprint measured it missing and every consumer
reaching the endpoint through the legacy alias. **If the central Redis moves,
both entries move together**, and the legacy one retires only when
kpidash-client's publishers have migrated (CD-3). `KDASH_CLAUDE_REDIS` was
seeded in sprint 003 at `rpidash2:6380` and flipped to `rpi53:6379` by the CD-7
cutover; sprint 006 gave it its first C consumer, so it is no longer a stem
with only writers.

**One walk, parameterized by stem** (sprint 006). The library resolves any
stem through `kdash_resolve_endpoint(&stem, ...)`, where a `kdash_stem_t`
carries the walk's two optional steps — the legacy alias and the compiled-in
fallback — as *data* rather than as branches. `kdash_resolve_central()` is that
call with `KDASH_STEM_CENTRAL` and keeps its name because every kpidash reader
already uses it. This is the C spelling of the `Stem` the Rust and Python
publishers have carried since sprint 003, and three implementations of one walk
agreeing is worth more than three that each hardcode their own.

A connection handle names its stem (`kdash_conn_opts_t.stem`, NULL meaning
central), so `claude:*` is read on a handle opened at `&KDASH_STEM_CLAUDE`.
**Both stems answer `rpi53:6379` today and are still resolved separately**: a
reader that took the shortcut would pass every test now and be wrong on the day
the family moves, which is the one thing the two stems exist to prevent.

**A compiled-in default is a reader's privilege, not a writer's.** The
`rpi53:6379` fallback exists because a dashboard with a stale-but-plausible
endpoint still renders, and a dashboard with none does not. A publisher's
arithmetic is the opposite: a misdirected write is a convincing record in the
Redis nobody is reading, and it looks like success from both ends. So the
central stem keeps its default for everyone, and `KDASH_CLAUDE_REDIS` — the
stem whose value is *changing* — has none: a publisher that cannot resolve it
drops the write and says so.

That asymmetry outlived the cutover, and it now applies to readers too. The
claude stem's `fallback` is NULL, so a resolve that finds nothing returns
`KDASH_EP_UNRESOLVED` rather than a guess: a panel rendering confident rows out
of the Redis nobody is writing to is a worse answer than one rendering
"unavailable" (CD-6). The central stem keeps its default, because "the central
Redis has always been at `rpi53:6379`" is still true and still useful.

## CD-5 — Consumers are read-only

Dashboards read feeds; they do not write to homelab services. The narrow
exceptions are dashboard-owned keys: their own local-Redis state, and
command/ack keys a feed family explicitly defines for them (e.g. kpidash's
status-ack). A dashboard never writes another family's keys.

## CD-6 — Degrade, don't block

An unreachable Redis is a degraded render, never a hung or dead dashboard:
lazy connect, bounded timeouts, swallowed failures surfaced as an
"unavailable" indicator, stale data rendered as stale. Published status is
trusted only while fresh (the kdeskdash claude-feed ladder — fresh → idle →
stale — is the model). This lands in the shared C library once (sprint 002)
instead of in three dashboards.

## CD-7 — `claude:*` migrates to the central Redis, as a korg program

The Claude-activity family is genuinely fleet-shared and will move from its
interim home (`rpidash2:6380`) to the central Redis. The move is **not a
single-sprint change** — the blast radius spans the publisher hooks and
statusline on every fleet host, kdeskdash env on both devices, AUTH coverage
for every writer, and feed freshness through the cutover — so it runs as a
**korg program**, with its gates and slices decided when the program is
proposed. Until that program completes, `rpidash2:6380` remains the family's
sanctioned interim home, and the family's schemas land with the move (or
sooner if a new consumer needs them).

The program was korg:1755 and it is **complete**. Four slices:

| slice | what it did |
|---|---|
| kdashdata 003 | publisher wrappers, the `KDASH_CLAUDE_REDIS` stem, CD-12's auth route |
| kdashdata 004 | `kdash-pub` distribution — store publish, fleet install (CD-13) |
| kdeskdash 031 | publisher cutover to a dual-write window, both readers repointed |
| kdashdata 005 | this close-out: old home retired, registry flipped, schemas landed |

What slice 1 bought was the *shape* of the cutover — the publishers stopped
being a fleet of hardcoded endpoints and became one khlenv store edit. Two
facts measured there set the size of the rest: `claude-pub.sh` carried a
hardcoded `192.168.1.144:6380` and sent **no AUTH at all**, and the central
Redis requires it. Repointing without CD-12 in place would not have degraded —
it would have silently published nothing.

Three things the program learned that outlive it:

- **A dual-write window is the cheap way to cut a feed over.** Publishers
  wrote both homes while readers moved one at a time, so no step in the
  sequence had a failure mode worse than "the new home is empty".
- **Pinning host and port does not pin a credential.** Repointing `claude:*`
  at an authenticated Redis gave kdeskdash's kvscf handle a password by
  inheritance, and a Redis with no password configured answers AUTH with an
  *error*. CD-8's warning could not have caught it: it said pin host and port,
  and host and port were pinned. Fixed in kdeskdash `6817457`, and generalised
  there as `a-fallback-outlives-the-sameness-that-justified-it.md`.
- **A pinned-version recipe is a second place a cutover lives.** k-homelab
  pins `kdeskdash_publisher_version` deliberately rather than following the
  store's `latest`, so publishing was never enough — until the pin moved, the
  next `bin/apply` would have reverted both managed hosts and silently stopped
  the dual-write.

## CD-8 — `kvscf:*` stays with its workstation pair

kvscf data is symbiotic between kvscf running on a workstation (cleo, kwork)
and the desk dashboard sitting in front of that workstation's keyboard
(rpidash2, rpidash3): a direct data **and control** exchange within one
desk pair, not fleet-shared state. It therefore does **not** move to the
central Redis — each pair keeps its own endpoint (dev pair on
`rpidash2:6380` today, work pair on its own instance). The nudge channel's
auth token and the pair-local scoping are features of this shape, not debt.

## CD-9 — Two dependencies, and no more: hiredis (system) + cJSON (vendored)

The shared C library (sprint 002) is where this repo stopped being
dependency-free, so the budget is stated rather than discovered:

- **hiredis — a system dependency.** Debian ships it (`libhiredis-dev`) on
  every architecture the dashboards run, both dashboards already link it,
  and vendoring a Redis client to avoid a package that is already installed
  everywhere would be the wrong trade. Found with **CMake config first,
  pkg-config as the fallback** — the fallback is not decoration: the aarch64
  sysroot ships `hiredis.pc` and no CMake config, so it is the path every
  cross build actually takes.
- **cJSON — vendored, in `lib/cjson/`.** Payloads are JSON, so something has
  to parse them; vendoring one MIT file pair keeps the cross build needing
  nothing in the sysroot but hiredis, and it is the same copy at the same
  version kdeskdash already carries. Vendored verbatim — never edited.

Anything further needs a decision here first. In particular, the library
speaks khlenv's HTTP protocol directly rather than taking libcurl for one
unencrypted GET (see CD-4); a resolver whose whole debugging story is
`curl -i` does not justify a dependency.

## CD-10 — The library is a pure core plus a thin I/O shell

The consumer library splits in two, and the split is load-bearing rather
than tidy:

- **Pure core** — key grammar, both freshness models, the five payload
  parsers. No Redis, no sockets, no ambient clock (`now` is always a
  parameter). It builds and tests on any host with a C compiler and
  nothing installed.
- **I/O shell** — endpoint discovery, the connection handle, the typed
  readers. Thin by construction: it fetches bytes and hands them to the
  core.

This is the kdeskdash `claude_feed.c` / `claude_redis.c` split, promoted to
a rule, and it is what makes the repo's gate honest: the logic that is
actually easy to get wrong — a key that should have been skipped, a required
field that should have rejected a record, a staleness boundary — is all
unit-testable with no Redis, no network, and no hardware. What is left
untested by `just check` is the socket code, which is verified live against
the real fleet (`just dump`).

The library has **no rendering of any kind** and never will: no LVGL, no
layout, no colours. Three dashboards with three different screens share a
data model, not a look.

## CD-11 — Publisher wrappers: two languages, one derivation, and they take the khlenv client

The publish side is `publishers/` — a Rust crate with a CLI, and a Python
package. Two languages because the publishers are two shapes, and the split is
by *latency*, not by taste:

- **Python** for daemon publishers (kpidash-client and friends): they already
  run an interpreter, they publish on a loop, and 100 ms of import cost is
  amortised over a process lifetime.
- **Rust, as a CLI** for shell publishers on a hot path — Claude Code hooks
  and statuslines, which fire on every prompt and every tool call. Measured on
  kai: a full publish costs **18 ms** through `kdash-pub` and **102 ms**
  through the Python package; `--version` alone costs 0.4 ms against 64 ms of
  import. That gap is the reason the CD-7 cutover vehicle is an exec, not an
  import.

**Both take the khlenv client rather than speaking its HTTP.** The C library
had no choice (CD-9: there is no C client, and libcurl for one unencrypted GET
was the worse trade). Rust and Python both have one, shipped by the khlenv repo
precisely so consumers do not re-derive it — and the endpoint precedence it
implements (`/etc/khlenv/endpoint`, `$KHLENV_ENDPOINT`, HKCU/HKLM on Windows)
plus the cache-file failover are a **cross-client contract** the two clients
pin together. A third implementation would be a third answer to "where is
khlenv", which is the one question that must have one.

The cost is stated rather than discovered: `khlenv-client` is a **git**
dependency on a private repo, so a first `cargo build` needs network and git
credentials for it. `just check-docs` stays free of that on purpose.

Dependency budget, publish side: Rust takes `khlenv-client`, `redis` and
`serde_json`; Python takes `khlenv` and `redis`. Argument parsing in the CLI is
hand-rolled — seven verbs and five flags do not justify a framework on a binary
that is exec'd once per tool call.

**CD-10's split applies here too**, and does more work than it does for the
reader. The key grammar, the `ts` rule, the env-file shape and CD-4's branching
are pure in both languages, which is what lets the Python half be gated with
**stdlib only** on a host where neither `redis` nor `khlenv` is installed. The
socket half is verified live instead (`just pub-endpoint`, the self-test).

The wrappers do **not** schedule, retry, or cap. A publisher owns its cadence
and its cap (rules.md); a wrapper that retried behind a hook's back would turn
a fire-and-forget into an unbounded stall.

## CD-12 — `REDISCLI_AUTH` reaches hook contexts through a 0600 env file

CD-2 says the password travels in `REDISCLI_AUTH`. What sprint 003 measured is
that *nothing delivers it* outside a systemd unit:

- Daemons get it from `EnvironmentFile=~/.config/kpidash-client/redis-auth.env`
  (0600) — the WI-255 fix, applied fleet-wide.
- `~/.kconfidential/api_tokens.fish` loads inside `config.fish`'s
  `status is-interactive` block, so it reaches **only an interactive fish
  session started by hand** — no ssh, no systemd, no agent.
- A Claude Code hook inherits neither. Measured on kai: `REDISCLI_AUTH` is
  UNSET in the Claude Code process and under `bash -lc`. `~/.claude/settings.json`
  has no `env` block, and putting a password in one would be worse than the
  gap.

`claude-pub.sh` works today only because `rpidash2:6380` is unauthenticated —
which is exactly why the CD-7 repoint cannot happen before this is settled.

**The convention**: when `REDISCLI_AUTH` is unset, a publisher reads it from a
0600 env file, in order — `$KDASH_AUTH_FILE`, `~/.config/kdash/redis-auth.env`,
then `~/.config/kpidash-client/redis-auth.env`. Same variable, same value, same
krot entry (`rpi53-redis-password`, krot #1293 — seven consumer copies); a
delivery mechanism, not a second contract, and no new secret is minted. The
kpidash-client file is last and is not kdashdata's to own, but it is already on
every reporting host and already on krot's consumer list, so the convention
works on day one with no new file anywhere.

Two properties that are load-bearing rather than tidy:

- **A group- or world-readable secret file is refused, not used.** The August
  2026 rotation missed the consumers whose copy lived in a different shape;
  a publisher that quietly used a 0644 secret would hide the same class of
  fault. Refusing is noisy in exactly the right place.
- **No password is still a valid answer.** "Found nothing" must connect anyway
  rather than error. Only a file that exists and cannot be trusted is fatal.
- **"I have a password" and "this server wants one" are different questions.**
  Measured against the interim claude home while building the wrappers: sending
  AUTH to a Redis with none configured is an *error*
  (`ERR AUTH <password> called without any password configured`), not a
  no-op — so a publisher that finds a password and always sends it cannot write
  to `rpidash2:6380` at all. The wrappers therefore carry an explicit opt-out
  (`--no-auth` / `without_auth()` / `authenticate=False`), which is what lets
  **one** publisher serve the CD-7 dual-write window across an unauthenticated
  and an authenticated endpoint. Explicit rather than a
  retry-without-password fallback: a silent retry would turn a *wrong*
  password into a connection that succeeds and then fails NOAUTH on every
  command, which is a worse failure than the one it papered over.

Both wrappers implement this; a future non-wrapper publisher that needs the
password in a hook context follows the same order or it is divergent.

## CD-13 — `kdash-pub` ships through the package store, to fixed absolute paths

Sprint 003 built the CLI and stopped there, so for one sprint `kdash-pub`
existed only as `publishers/rust/target/release/kdash-pub` in this checkout on
kai. That is not a distribution, and the CD-7 cutover (which needs the binary
on **kai, kubs0 and cleo**) could not start. This decision closes it, and
takes nothing new: the fleet settled binary distribution in kpolice sprint 003
and the krcmd rollout — package store on kubsdb, `knarr` for Linux, a
store-resolving PowerShell installer for cleo.

**One version, two artifacts.** `just publish` builds the Linux binary and the
`x86_64-pc-windows-gnu` cross-build from one checkout and publishes both under
one `kpkg` version and one `SHA256SUMS`. Never two versions: a fleet that
resolves `latest` differently per platform is a fleet that drifts. The
cross-compile is available because `kdash-pub` is pure Rust — the lockfile
carries no `-sys` crate, and `winreg`/`windows-sys` need the mingw linker and
nothing else — so cleo builds nothing.

**The stamp and the store label are one fact.** `build.rs` emits
`0.1.0-<describe> (<date>)` and `--version` prints it verbatim; `just publish`
re-reads the built binary and publishes under exactly that second field, and
knarr's confirm step re-reads the *installed* binary the same way. A
`0.1.0 (abc1234)` shape would fail that confirm on a perfectly good install.
A `dirty` or `unknown` stamp is refused at publish: a published version must
name a commit someone can check out. Off `main`, `--no-latest` — a branch
commit vanishes at squash-merge, so a branch build may exist to prove a path
but must never be what the fleet resolves.

**The install paths are a contract, not a convenience:**

| host | path | installed by |
|---|---|---|
| kai, kubs0 | `/usr/local/bin/kdash-pub` | `knarr deploy` (`just deploy`) |
| cleo | `C:\tools\bin\kdash-pub.exe` | `scripts/install-cleo.ps1` (`just deploy-cleo`) |

CD-12 is the reason. A Claude Code hook context inherits neither an
interactive shell's environment nor its `PATH`, so the hook scripts that will
call this binary exec it by absolute path — `/c/tools/bin/kdash-pub.exe` under
Git Bash on cleo. Moving either path is a change to those hooks.

A private per-user copy beside each hook script was considered and rejected
during the CD-7 program: it would bypass the store, so a `just publish` +
`just deploy` upgrade would never reach the hooks, recreating stale-binary
drift in a per-user form invisible to knarr and kmuster.

**Verify by naming the hosts.** `just deploy-all` covers all three, and the
verification names kai, kubs0 and cleo explicitly rather than iterating
whatever the runner reached — that is precisely how kpolice sprint 002 left
cleo on a commit that no longer existed for a whole sprint.

## CD-14 — `kdash-pub` has exactly one read verb, and it is a publisher's

Sprint 003 drew the boundary as "no reading": the consumer side is `libkdash`,
and that stays true for *consumption*. The data model, the freshness ladder and
the skip-a-bad-record discipline live in the C library, and a second copy of
them in Rust would be a second contract to keep in step.

But a publisher has a read that is not consumption. `claude:limits` is one
shared key with writers on several hosts, and a poll writer must not publish
over a fresher observation — so it reads `updated_at` back before writing.
kdeskdash sprint 031 left that as the last hand-rolled RESP request over
`/dev/tcp`, aimed at the unauthenticated interim home because that was the only
endpoint bash could reach without re-deriving CD-12's auth rules. Correct for
the dual-write window, and wrong the moment the stem flipped to an
authenticated Redis (korg:1769).

The alternative was re-hand-rolling AUTH in bash, including the
refuse-a-group-readable-file rule — precisely what CD-11 built the CLI to stop.
So `kdash-pub` gained `hget <key> <field>`, on the binary that already has
khlenv resolution, CD-12 auth, the key grammar and the connection code. The
read was the small part.

The boundary that replaces "no reading":

- **One field, no model.** No decoding, no freshness verdict, no notion of a
  record — it returns bytes.
- **The same key grammar,** through the same choke point as every write. A key
  no reader will parse is not one a publisher may ask about either.
- **The same exit codes.** 0 with the value; 0 with *nothing* on stdout when
  the field is absent, because a missing field is an answer and not a fault;
  2 for delivery failure. `--best-effort` therefore degrades a read to
  "unknown" exactly as it degrades a write to "dropped", and a caller keeps one
  error convention across both.
- **Not a consumer API.** Anything that wants a *feed* — sessions, a staleness
  decision, anything rendered — uses `libkdash`. A second read verb needs a
  reason of the same shape: a publisher that cannot write correctly without it.

The Python wrapper does not have this and does not need it; it gains one when
something it publishes needs a guard, on the same evidence-first footing as the
rest of this repo's wrapper surface.

## CD-15 — HASH-shaped feeds parse from a field/value list, not a buffer

Every kpidash feed is a JSON document under one key, so `kdash_payload.c` was
entirely cJSON-over-a-buffer. `claude:session:*` and `claude:limits` are Redis
**HASHes** — the registry's first — and their schemas describe the *decoded*
record, with every value arriving off the wire as a string.

The parser signature follows the shape of the data rather than being forced
into the existing one:

```c
bool kdash_parse_claude_session(const char *host, const char *sid,
                                const char *const *fields,
                                const char *const *values, int nfields,
                                kdash_claude_session_t *out);
```

The I/O shell flattens an HGETALL reply into two borrowed pointer arrays and
hands them over; the pure core does the rest and stays testable with no Redis.
`claude:recent` really is JSON per list element, so it takes a buffer like
every other parser — one family, two shapes, and the shape is the schema's, not
a convention imposed on it.

**Every rule from [rules.md](../contracts/rules.md) survives the change of
shape**: a required field missing or malformed rejects the record whole, a
malformed optional is treated as absent, unknown fields are ignored, `*out` is
zeroed before the parse. What is genuinely new is that the "is this a number"
question cJSON answered for free must now be asked explicitly, and strictly — a
hash value of `"later"` must reject rather than read as zero.

Two consequences worth stating for the next HASH family:

- **The field cap is set above today's field count on purpose.** Both schemas
  are `additionalProperties: true`, and a cap sitting exactly on the known
  fields would start silently dropping *required* ones — and so rejecting valid
  records — the day a writer adds another. The reference implementation's cap
  of 16 sat exactly on `claude:limits`' 16 documented fields; this one is 32.
- **A partial hash can be a legitimate transient.** A `claude:session` carrying
  only `ts` is a keepalive that landed before the first full write, and
  rejecting it whole is correct — it is also the resurrection-race guard, since
  a statusline write after SessionEnd leaves exactly that shape.

## CD-16 — The claude derived model: derivation and ordering in, formatting out

kdeskdash's pure core does more than parse: it derives a 5-state display status
from published status × age, sorts sessions attention-first, and formats ages
and reset times. Sprint 006 split that at a deliberate line.

**In the library**, because they are the data model and two dashboards
answering them differently is drift:

- the 5-state derivation (`kdash_claude_display`) and the attention-first sort
  (`kdash_claude_sessions_refresh`);
- the **thresholds** — 15 min idle, 40 min stale, and the limits rule of "own
  stamp + writer cadence + grace, legacy fixed window when no cadence was
  published, scoped set never borrowing headline freshness". "When is a session
  probably killed" has one right answer for the fleet, not one per panel;
- lowercase enum words (`kdash_claude_disp_str`), which are the schema's own
  vocabulary — the same thing `kdash_service_state_str` returns.

**In the panel**, because CD-10 says the library has no rendering of any kind:
`cf_disp_label`'s uppercase strings ("BLOCKED ON YOU"), `cf_fmt_age` ("3m"),
`cf_fmt_reset` ("Tue 07:00"), and any placeholder for an absent `project`.

Two rules that make this reusable rather than one family's special case:

1. **The derivation composes on `kdash_ladder()` rather than beside it.** CD-6's
   3-state ladder owns the time bands; the published status only decides what a
   non-stale band renders as. The next HASH-shaped feed reuses the ladder
   instead of writing a sixth state machine.
2. **Thresholds are parameters, with the constants as defaults** — the shape
   `kdash_ladder(age_s, idle_s, stale_s)` already had. Policy that cannot be
   named at the call site is policy nobody can see.

One deliberate divergence from kdeskdash's `cf_display_status`, kept because it
is a behaviour and not an accident: **only `working` degrades to idle.**
`blocked` and `awaiting` both mean "your turn", and a turn does not stop being
yours because you took twenty minutes over it, so they stay prominent until the
ladder says stale. Age still has the last say at the stale boundary, because
the hooks cannot report a killed process.

## Open questions

- **OQ-2 — Redis ACL writer/reader split** (see CD-2).
