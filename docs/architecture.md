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

Exception today: the Claude-activity family lives on `rpidash2:6380`
pending its migration to central (CD-7); the kvscf family stays off-central
by design (CD-8).

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
  (sprint 003). Answers `rpidash2:6380` today and the central Redis after the
  CD-7 cutover; that flip is the whole reason the stem exists.
- **Local Redis needs no discovery** — it is `127.0.0.1:6379` by definition;
  per-app env overrides (e.g. `KDESKDASH_REDIS_HOST`) stay as-is.
- Feed families not on the central Redis (see OQ-1) keep their per-app env
  configuration until their home is settled.

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
seeded in sprint 003 at `rpidash2:6380`, with no consumer yet — it is inert
until the cutover slice points the publishers at it.

**A compiled-in default is a reader's privilege, not a writer's.** The
`rpi53:6379` fallback exists because a dashboard with a stale-but-plausible
endpoint still renders, and a dashboard with none does not. A publisher's
arithmetic is the opposite: a misdirected write is a convincing record in the
Redis nobody is reading, and it looks like success from both ends. So the
central stem keeps its default for everyone, and `KDASH_CLAUDE_REDIS` — the
stem whose value is *changing* — has none: a publisher that cannot resolve it
drops the write and says so.

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

The program is korg:1755, and sprint 003 was its first slice: the publisher
wrappers, the `KDASH_CLAUDE_REDIS` stem, and CD-12's auth route. What that
buys is the shape of the cutover — the publishers stop being a fleet of
hardcoded endpoints and become one khlenv store edit. Two facts measured in
that sprint set the size of the remaining work: `claude-pub.sh` carries a
hardcoded `192.168.1.144:6380` and sends **no AUTH at all**, and the central
Redis requires it. Repointing without CD-12 in place would not degrade — it
would silently publish nothing.

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

## Open questions

- **OQ-2 — Redis ACL writer/reader split** (see CD-2).
