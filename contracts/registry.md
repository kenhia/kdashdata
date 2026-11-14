# Feed registry — contract v0 (current reality)

> The inventory of every Redis feed the dashboards touch, as it exists
> today. Shapes here are **grandfathered** (CD-3): documented as-is, no
> renames. New feeds follow [rules.md](rules.md). Family status is `live`,
> `migrating`, or `retired`.

## Homes

Endpoints are **resolved, never hardcoded** (CD-4): the khlenv stem is the
address a publisher or consumer actually knows. The value column is what the
stem answers today, and a move is an edit to the store, not to any consumer.

| Home | khlenv stem | Value today | Auth | Holds |
|---|---|---|---|---|
| Central | `KDASH_CENTRAL_REDIS` (legacy alias `KPIDASH_REDIS`) | `rpi53:6379` | `REDISCLI_AUTH` (krot: rpi53-redis-password) | `kpidash:*`, `kdash:*` |
| Claude feed (CD-7, relocated) | `KDASH_CLAUDE_REDIS` | `rpi53:6379` | `REDISCLI_AUTH` (krot: rpi53-redis-password) | `claude:*`, `ghcp:*` |
| Workstation pair (CD-8, amended) | per-app env (no stem yet) | **dev pair: central, `rpi53:6379`** (sprint 014); work pair its own | per-app env; the dev pair's half now uses the central `REDISCLI_AUTH` | `kvscf:*` — the work pair stays with the pair, because kwork cannot reach central |
| Dashboard-local | none — `127.0.0.1:6379` by definition | `127.0.0.1:6379` on each dashboard host | none/local | `<dashboard>:*` |

**Two stems, one endpoint, deliberately.** `KDASH_CLAUDE_REDIS` and
`KDASH_CENTRAL_REDIS` both answer `rpi53:6379` now that the CD-7 relocation is
complete (program korg:1755, sprint 005). Collapsing them into one name would
spend the thing that made the move a single store edit: the claude family keeps
its own address, so it can move again without a publisher host being touched.
The relocation itself was that edit — one line in khlenv's store, plus one
`KDD_LEGS` value in the publisher.

The C consumer library honours the split rather than inheriting it on paper:
`kdash_conn_opts_t.stem` names which stem a handle resolves, and the claude
readers require a handle opened on `&KDASH_STEM_CLAUDE`. Reading `claude:*`
through the central stem would work today and break at the next move — which is
the whole point of keeping two names for one address.

`kvscf:*` deliberately has **no** stem, and that pin is what makes the sprint
014 split expressible at all: both panels pin `KDESKDASH_KVSCF_REDIS_HOST/PORT`
explicitly since kdeskdash sprint 031, so rpidash2's half can be repointed at
central while rpidash3's stays on its own instance, by configuration on each
panel rather than by a shared default. That pin is not a formality — the
credential inherited across a differing endpoint is what actually broke during
the claude repoint (see the claude family below), and it is the same trap the
dev pair's move walks past: on central the password is the fleet's, on
rpidash3 it is that instance's own.

## Family: kpidash (central, live)

Owner: the kpidash repo. Detailed payload reference during transition:
`kpidash/docs/CLIENT-PROTOCOL.md` (v2.0) — the five core payloads are
schema'd here (linked below) and those schemas are authoritative; the
remaining kpidash-internal shapes stay with CLIENT-PROTOCOL.md until
migrated.

### Cross-dashboard feeds (schema'd in v0)

| Key | Type / pattern | Writer (cadence) | TTL | Schema |
|---|---|---|---|---|
| `kpidash:clients` | SET of hostnames | kpidash-client (startup, `SADD`) | none | — (bare hostnames) |
| `kpidash:client:{host}:health` | latest-value, expiring | kpidash-client (~3 s) | 5 s | [health](schemas/kpidash-client-health.schema.json) |
| `kpidash:client:{host}:telemetry` | latest-value, expiring | kpidash-client (~5 s) | 15 s | [telemetry](schemas/kpidash-client-telemetry.schema.json) |
| `kpidash:client:{host}:dev_telemetry` | latest-value, expiring | kpidash-client (~1 s) | 5 s | [dev_telemetry](schemas/kpidash-client-dev-telemetry.schema.json) |
| `kpidash:services:{name}:{host}` | latest-value, ts-owned | any publisher / CLI | none | [service_status](schemas/kpidash-service-status.schema.json) |
| `kpidash:apttemps:{zone}` | latest-value, ts-owned | apt-temps publisher | none | [apttemps](schemas/kpidash-apttemps.schema.json) |

Notes: key absence (TTL expired) = source offline for the expiring three.
**`health` may carry `availability: "intermittent"`** (kpidash sprint 023,
listed here in sprint 018): a host that sleeps by design says so, and a reader
that remembers the last value past expiry can draw that host grey *asleep*
instead of red *offline*. Optional and additive — absent means always-on, and
no pre-023 payload changed. kpidash's `docs/CLIENT-PROTOCOL.md` §2 owns the
semantics. libkdash's `kdash_health_t` does not surface the field yet, so a C
consumer sees such a host as offline like any other.
`services` keys are exactly 4 segments, `_` = "no host"; non-conforming keys
are ignored. `services`/`apttemps` staleness is reader-owned via `ts`, and **the window
is the consumer's, per service** — not a number this contract fixes. kpidash's
default is 60 s (`apttemps` 300 s), and a *daily* publisher gets its own: since
kpidash sprint 018 `kmon` has a 26 h window, and `kpidash:services:kmon:kai` is
published once per nightly. Read kpidash's `docs/CLIENT-PROTOCOL.md` §8a
("Freshness windows") for the live table rather than a number restated here.
The payload deliberately carries **no cadence field** — the shape has two
consumers and adding one was declined (handoff korg:2309); a publisher whose
cadence is not the default tells its consumer, not its readers. A red card
means "nobody is publishing", not "that host is down" — check the publisher.

**Bands (`apttemps`).** What counts as cold / ok / warm / hot is part of the
shared data model, not each panel's taste: `kdash_apttemps_band()` in
[`kdash_payload.h`](../include/kdash/kdash_payload.h), with the thresholds
`KDASH_APTTEMPS_{COLD,OK,HOT}_F` in
[`kdash_freshness.h`](../include/kdash/kdash_freshness.h) as defaults. Below
65 F cold, 65–75 ok, 75.1–79.9 warm, 80 and above hot — kpidash's bands,
which kstudiodash (retired with kstudio, 2026-10-20) had ported by hand. Which *colour* a band renders as stays
in the panel (CD-10). A zone whose `ts` is outside the 300 s window classifies
`stale` regardless of the reading. Added sprint 009 (CD-16's argument applied
to `apttemps`, WI 1804).

**Counted readers return -1 or a complete list** (sprint 009, WI 1790).
`kdash_services()`, `kdash_apttemps()` and `kdash_claude_sessions()` SCAN and
then read each key, so they can lose the endpoint mid-list. When that happens
they return -1 with `out` zeroed rather than the rows gathered so far: a
partial list is indistinguishable from a complete one at the call site, and
these feeds render missing rows as absent *things*.

### kpidash-internal (documented for completeness, not schema'd)

| Keys | Pattern | Notes |
|---|---|---|
| `kpidash:status:current`, `kpidash:status:ack:{id}` | latest-value + ack key (EX 60) | the CD-5 sanctioned client-write example |
| `kpidash:cmd:*` | dev/control toggles, EX 300 | grid, textsize, services:evict (GETDEL) |
| `kpidash:screenshot` | one-shot command (GETDEL) | device self-screenshot |
| `kpidash:system:*` | diagnostics | logpath/version plain strings; mem:current + mem:ring (LPUSH, trim 1500) |

**Retired from this table** (kdashdata 022, korg:4598): `kpidash:activities` /
`kpidash:activity:{uuid}`, `kpidash:repos:{host}`, `kpidash:fortune:current` /
`kpidash:fortune:pushed`, and the `graph` (`kpidash:cmd:graph`,
`kpidash:cmd:graph:{host}`) and `fortune_dev` commands under `kpidash:cmd:*`.
kpidash sprint 032 (korg:4562) removed every reader; kpidash-client 2.0.0,
sprint 036 (korg:4596), removed the last writers and deleted the keys on
central. The shapes are on kpidash's git tag `last-with-dev-graphs-fortune`.

## Family: kdash (central, live)

Owner: kdashdata. The namespace new shared feeds land in (rules.md); the
first key arrived with the publisher wrappers in sprint 003.

| Key | Type / pattern | Writer (cadence) | TTL | Schema |
|---|---|---|---|---|
| `kdash:selftest:{host}` | latest-value, expiring | either publisher wrapper, on demand | 300 s | [selftest](schemas/kdash-selftest.schema.json) |
| `kdash:panel:{host}` | latest-value, ts-owned | kdeskdash, on demand (a button press) | none | [panel](schemas/kdash-panel.schema.json) |
| `kdash:panelmode:{host}` | latest-value, ts-owned | any operator or dashboard, on demand | none | [panelmode](schemas/kdash-panelmode.schema.json) |
| `kdash:panelshot:{host}` | latest-value, ts-owned | `kddss` and any operator, on demand | none | [panelshot](schemas/kdash-panelshot.schema.json) |
| `kdash:stale:{host}:{deployer}` | latest-value, presence-owned | each deployer, on a skipped deploy | none (never) | [stale](schemas/kdash-stale.schema.json) |
| `kdash:agentact:{host}:{sid}` | latest-value, expiring | klaude-top `--publish` (~2 s tick) | ~10 s | [agentact](schemas/kdash-agentact.schema.json) |
| `kdash:net:{host}` | latest-value, expiring | kpidash-client / kpidash-client-win (~5 s) | 15 s | [net](schemas/kdash-net.schema.json) — **v1** |
| `kdash:net:ack:{host}` | latest-value, expiring | Ken or an agent, on demand (`SET … EX 86400`) | 24 h | [net-ack](schemas/kdash-net-ack.schema.json) |
| `kdash:update:{host}:{kind}` | latest-value, presence-owned | k-homelab's apt runner, per run (not yet — see below) | none (never) | [update](schemas/kdash-update.schema.json) |
| `kdash:nas:{host}` | latest-value, expiring | kpidash-client on the NAS host (not yet — see below) | 3× cadence | [nas](schemas/kdash-nas.schema.json) — **v1** |
| `kdash:weather:{loc}` | latest-value, ts-owned | kweather on kai, every 10 min (not yet — see below) | none | [weather](schemas/kdash-weather.schema.json) — **v1** |
| `kdash:korg:pulse` | latest-value, ts-owned | korg's `bin/korg-pulse` on kai, every 5 min | none | [korg-pulse](schemas/kdash-korg-pulse.schema.json) — **v1** |
| `kdash:klams:status` | latest-value, ts-owned | klams-monitor on kubs0, every 60 s | none | [klams-status](schemas/kdash-klams-status.schema.json) — **v1** |
| `kdash:calendar` | latest-value, ts-owned | korg's `korg-calendar` timer on kai, on change and at least hourly | none | [calendar](schemas/kdash-calendar.schema.json) — **v1** |
| `kdash:agents` | latest-value, ts-owned | ksitrep on kai, every 60 s (not yet — see below) | none | [agents](schemas/kdash-agents.schema.json) — **v1** |

`selftest` is a publish canary, not a dashboard feed: running it from a host
proves that host's whole publish path — khlenv discovery, `REDISCLI_AUTH`, key
grammar, schema-valid payload — and nothing renders the result. Key absence
means nobody has run it there lately, which is not a fault.

`panel` is the family's first **control** feed (CD-17): it tells the dashboard
running on `{host}` which screen to show — `dash` or `desktop`. Its only
reader was kstudiodash, which acted on it by switching VT; kstudio left the
homelab on 2026-10-20, so the family has **no reader today** (see the
retirement note below the command table). Deliberately *state* rather than the
`GETDEL` one-shot command pattern [rules.md](rules.md) also offers, for two
reasons that both point the same way. Consuming a one-shot is a **write**, and
the consumer library has no write path by design (CD-5). And the consumer must
act on `ts` **advancing**, never on the current value: that makes a republish
idempotent, and it stops a `want` sitting unchanged in Redis yanking a panel
back after someone switched screens by hand. A command older than the reader's
window (60 s, `KDASH_PANEL_WINDOW_S`) is not replayed at all, so a panel that
was off for an hour comes back to its own screen rather than to whatever it was
told while it was away.

**C reader**: `kdash_panel()` in `include/kdash/kdash_feed.h` — one GET on an
ordinary central-stem handle, with the edge detection in
`kdash_panel_actionable()` (`kdash_payload.h`), so two panels cannot disagree
about what "a new command" means. **Writer**: no new publisher code. The key is
in the `kdash` namespace the wrappers already accept and `ts` is stamped for
free ([rules.md](rules.md) payload rules), so the whole write is:

```sh
kdash-pub set kdash:panel:<host> '{"want":"desktop"}'
```

`panelmode` and `panelshot` are `panel`'s **siblings**, added in sprint 014 so
kdeskdash can be commanded from central once its local Redis retires (program
korg:2935). One family per *verb*, because a `ts` is a command's identity and
two commands under one stamp would share one edge (CD-22). All three inherit
CD-17's rule verbatim — act on `ts` advancing, never on the current value,
never replay a command older than the 60 s window — and each keeps its own
`acted_ts`.

| key | says | to |
|---|---|---|
| `kdash:panel:{host}` | show the dashboard, or yield the display (`want`: `dash` \| `desktop`) | no reader today (was kstudiodash) |
| `kdash:panelmode:{host}` | show this screen, optionally configured like this | kdeskdash today |
| `kdash:panelshot:{host}` | capture the screen, optionally to this path | kdeskdash today |

**`kdash:panel:kstudio` is retired** (WI 3986). It was the one instance of the
family ever written, and its host left the homelab on 2026-10-20 (given away;
k-homelab korg:3988). Central held neither it nor any `kdash:stale:kstudio:*` by the
time this was written (SCAN from kai, 2026-10-29, with a positive control). The *family* stays: the schema, `kdash_panel()`
and the CD-17 rule are a working control channel any future dashboard host can
take up with no contract change, and retiring a C reader is an API removal in
two vendoring repos, not housekeeping.

`want` is a **closed** enum and `mode` deliberately is not. They are two axes:
`want` is the coarse question every dashboard host has (is the dashboard on
the screen at all), `mode` is which screen *within* the dashboard, and its
vocabulary is the dashboard's own (`clock`, `gol`, `golz`, `dev`, `calc` on
kdeskdash). A dashboard ignores a mode it does not have; the contract
validates the shape and never the meaning (CD-22).

`panelmode`'s optional `settings` object is the same apply-the-fields-present
injection the `kdeskdash:gol:settings` / `kdeskdash:golz:settings` HASHes
carried, moved onto the wire: names and meanings belong to the mode, values
are text as a HASH's were, and a name absent keeps whatever the mode would
have used. It rides the mode command because settings are always *a mode's*
settings — naming the mode already showing injects without switching.

`panelshot`'s `path` is optional and **absolute**, so it can never resolve
against the dashboard's working directory; the panel owns the rest of the
policy and should refuse a path outside a directory it owns (CD-23).

**C readers**: `kdash_panelmode()` and `kdash_panelshot()` in
`include/kdash/kdash_feed.h`, one GET each on an ordinary central-stem handle.
The edge rule is `kdash_cmd_actionable()` (`kdash_payload.h`) — the generic
form of `kdash_panel_actionable()`, which now calls it, so all three families
answer "is this a command I have already acted on" the same way.
**Writer**: no new publisher code, exactly as `panel` needed none:

```sh
kdash-pub set kdash:panelmode:rpidash2 '{"mode":"golz","settings":{"density":"0.35"}}'
kdash-pub set kdash:panelshot:rpidash2 '{"path":"/var/tmp/kdd.bmp"}'
```

`stale` is the family's first **fleet-state** feed — data about the fleet
rather than about a dashboard — and the first whose freshness rule is neither
of the two [rules.md](rules.md) offers (CD-18). One key per *(host, deployer)*
pair records that this deployer wanted to push to this host and could not:
`fleet-deploy` sets `kdash:stale:komarchy:agent-skills` when it skips komarchy,
k-homelab's `bin/apply` sets `kdash:stale:komarchy:k-homelab` beside it, and
each deletes **only its own** after a verified sync. The pairing is the whole
point — "what is out of date on that machine" is the operator's real question,
and a single per-host flag could not answer it without one deployer's success
erasing another's failure.

`{host}` is the host's name in **k-homelab's `inventory.yml`** — the file that
declares `availability: intermittent` — not a manifest name and not a dashboard
name. `{deployer}` is the pushing repo. Both segments take the ordinary
host-token grammar from [rules.md](rules.md), and the family is exactly 4
segments.

**The key's presence is the flag**: `SET` on a skip, `DEL` on a verified sync,
no TTL — and, unlike the ts-owned feeds above it, **no staleness window**. A
reader must never age this record out. `services` and `apttemps` age out
because a silent writer there means "nobody knows"; here a silent writer means
the host is *still* unreached, so expiring the flag would make the longest
outages show the greenest dashboards. A deployer that never runs again
correctly leaves its flag up forever.

That inverts a second default, and it is worth stating because every other
reader in this repo does the opposite: **a malformed payload must not clear the
flag.** Elsewhere an unparseable record is skipped whole ([rules.md](rules.md),
Payloads); skipping this one renders as all-clear. A reader that cannot parse
the payload still reports the host stale and simply drops the detail — the
payload only ever enriches a signal the key has already given.

**Two stamps, and neither is a last-contact time.** `since` is the *first* skip
of the current run — how long this has been broken, and it must be carried
unchanged across later skips rather than restamped, or "stale for three weeks"
silently becomes "stale for an hour". `ts` is the *last* write, the most recent
skipped run. The host's actual last-contact clock is a third thing that lives
outside this feed: k-homelab keeps it per-checkout as a git-ignored
`.state/last-seen`, deliberately not in Redis so `bin/audit` never blocks on
it. After a three-week outage `since` and `last-seen` differ by the whole
outage, so operator-facing text built from the wrong one is wrong by exactly
the number that mattered.

`stale` is pinned `const: true` in the schema rather than merely documented as
always-true. It is redundant with the key's presence and exists so a human
running `GET` reads a sentence; there is no `false`, because clearing is a
`DEL`. A writer publishing `stale: false` has not cleared anything — it has
left the flag up and gone off-contract, and the schema says so structurally
instead of trusting prose.

**Writers**: agent-skills `fleet-deploy` and k-homelab `bin/apply` / `bin/audit`,
each owning only its own `{deployer}` key. No new publisher code — `kdash` is
already in the namespace table both wrappers accept, `ts` is stamped for free
([rules.md](rules.md) payload rules), and `del` is an existing verb, so both
halves are one line each:

```sh
kdash-pub set kdash:stale:komarchy:k-homelab \
  '{"stale":true,"since":1786698720,"reason":"bin/apply skipped: unreachable"}'
kdash-pub del kdash:stale:komarchy:k-homelab
```

**The read-modify-write is a third line, and it is `get` (sprint 015, CD-14
amended).** `since` must be read back and carried unchanged across every later
skip: a writer that restamps it turns "stale for three weeks" into "stale for
an hour", which looks exactly like working code. The three outcomes are the
contract — absence is the only all-clear here, so "I could not ask" must never
be read as "not set":

```sh
if since=$(kdash-pub get "kdash:stale:$host:$deployer"); then
    since=$(printf '%s' "$since" | python3 -c 'import json,sys; print(json.load(sys.stdin)["since"])')
elif [ $? -eq 1 ]; then
    since=$(date +%s)          # genuinely the first skip
else
    exit 2                     # Redis unreachable — do NOT restamp
fi
```

Before these verbs a writer dropped to `redis-cli` for that one read, and to
reach Redis at all had to reimplement the khlenv stem lookup and the CD-12
credential order. Two copies existed and had already drifted (k-homelab's
never tried `/etc/khomelab/secrets.env`) — see WI 1929 and k-homelab WI 2546.

The wrappers validate the namespace and the token charset, **not** this
family's segment count — `kdash:stale:komarchy` with the deployer left off
publishes without complaint and no reader will ever look at it. Arity is the
reader's choke point by design ([rules.md](rules.md), Key grammar), so a
deployer builds this key carefully or not at all.

**Readers**: kpidash renders a `<host> stale` card while any key for that host
exists, listing the deployers behind it — except that for an
`availability: intermittent` host the card is held back until that host's
**oldest** `since` is **more than 7 days** old, the same window as kmon's
`KMON_ABSENCE_DAYS` (k-homelab korg:4023 decision 2(i), kpidash sprint 026). The
grace is **reader-side only**: the key is set on the first skip exactly as above,
stays as truthful as before, and no writer waits a week or learns availability.
An unreadable `since` voids the grace rather than extending it. kmon may later read it instead of
counting an intermittent host against fleet health. No C reader ships with this
contract — the feed is registered ahead of its consumers on purpose. The day a
dashboard wants it through libkdash it needs a **SCAN over
`kdash:stale:{host}:*`** and a parse (both identity segments come off the key),
not the single GET the other two `kdash:*` feeds use. A non-C reader has that
as `kdash-pub scan 'kdash:stale:<host>:*'` or Python `Publisher.scan(...)`
(sprint 015) — and **no keys matched is exit 0 with no output**, distinct from
"could not ask", which is exit 2. For a presence-owned feed those two are the
difference between "this host is fine" and "nobody knows", so they are
different answers rather than one empty one.

`agentact` is the family's first **observed-from-outside** feed, and the first
whose key is deliberately a *copy* of another family's (CD-20). Everything else
here is a thing reporting on itself: a client publishes its own health, a
deployer publishes its own skip, a session's hooks publish that session's
status. This one is a process monitor — klaude-top, reading `/proc` on the
host — publishing its judgement of an agent that is not consulted. That is the
entire value: **an agent that has hung cannot publish that it has hung.**
`claude:session` goes quiet in exactly the same way whether a session is
thinking hard, waiting for Ken, or wedged; `kdash:agentact` is the second
opinion that tells those apart, which is what makes an overseen karc leg on kai
or kubs0 legible while it runs.

The four verdicts are klaude-top's, carried verbatim in its own uppercase:
`WAITING` (turn ended, the operator's move), `WORKING`, `TOOL` (quiet at the
top level but a child is alive — a long tool call), `STALLED` (owes a reply,
quiet, nothing running). Only `verdict` and `ts` are required; a tick that
could not read a counter publishes what it has, because a partial liveness
signal is still liveness.

**`name` is the row's label, and for a karc leg it is the leg's own name.**
klaude-top derives it from the `-n <name>` karc gave the session, falling back
to the cwd basename — so an overseen leg publishes as `overseen 2747` rather
than as a pid on a host, and the row Ken is looking at is identifiable as the
leg the overseer launched. That is what this feed is for, so a panel should
treat `name` as the first-class display field even though the schema marks it
optional. `transcript` rides along beside it as the host-local path klaude-top
found: diagnostic only, meaningless off-host, and worth knowing about mainly
because `{sid}` is *derived* from it (basename minus `.jsonl`), which makes it
look like an identity it is not — the key stays authoritative.

**The key is the join, and the join is verified rather than assumed.**
`{host}` and `{sid}` are the same two tokens as
`claude:session:{host}:{sid}`, so a reader holding one row has the other's key
already — no lookup, no mapping table, no third key to keep in step. It works
because the identity is genuinely one thing wearing two hats: the hooks key on
Claude Code's `session_id`, and the transcript file klaude-top locates is named
`<session_id>.jsonl`, so the uuid klaude-top takes off the filename *is* the
hooks' `{sid}`. Confirmed live on kai in sprint 012 against a running session.
The two families stay independently publishable — either can be absent — and a
reader must treat a missing `agentact` row as *unknown*, never as idle: key
absence here means no monitor is running on that host, which is not a statement
about the agent.

**Key absence is deliberately weaker than on the other expiring feeds**, which
is why it is worth spelling out. For `kpidash:client:*` absence means the
source is offline and the card goes red. Here it means nobody is watching —
klaude-top is Linux-and-`/proc` only, so **cleo will never write this feed**,
and a panel that rendered absence as "no agents on cleo" would be asserting
something it cannot know. That is a fact for the registry, not a gap to close.

**Writer**: klaude-top, through `kdash-pub` — `kdash` is already in the
namespace both wrappers accept and `ts` is stamped for free, so no new
publisher code. klaude-top's `--json` snapshot is already the payload, in the
right units (`ts` is unix seconds float there too) and with `snake_case`
names, so the contract is its `Snapshot` struct rather than a translation of
it. **Readers**: kxeneon's Agents panel reads Redis directly. No C reader ships
with this contract — the feed is registered ahead of its consumers, as `stale`
was.

Two shapes in the schema exist to stop a specific misreading. `transcript_age`
is explicitly nullable: `null` means klaude-top looked for a transcript and
found none, which is a different fact from the field being absent, and a reader
coercing it to `0` reports a hung session as having just written.
`transcript_growth` has no minimum, because a rotated or truncated transcript
makes it negative — the bookkeeping moved, not the work. The optional
`cpu_hist` / `in_hist` arrays are a **slot the contract opens, not a field to
expect**: klaude-top marks its history slices `json:"-"` today, terminal-UI
only, so a panel wanting a sparkline either keeps its own ring or asks
klaude-top to start emitting them. Capped at 24 either way — an uncapped array
in a value republished every two seconds is how a Redis fills up quietly.

`net` is the family's first **network** feed: one host's view of the 10G
fabric and of its own way out of it (sprint 019, CD-27). Its consumer was
fixed before the contract was written: kpidash's 2×1 network-health card,
design "B · Rail" (korg:4059). **Every field is one that card draws**, and that
is the rule for adding one. The card shows, per host column, the leg carrying
traffic and its speed, a sparkline of fabric throughput with a tick per link
drop, `LFC n (+Δ)` / `rx err n`, and UDM · PEERS · WAN pills, with WAN RTT and
feed age underneath.

**Each host probes its own path.** That is what makes the fabric-uplink
failure visible: every server can reach its peers and none can reach the UDM
(korg:4055). A central prober would see one path and miss it. So `reach` and
`rtt_ms` are measured *from* `{host}`, and there is **no fabric-level key**.
The panel derives the uplink state (every host `peers`, no host `udm`), and
no two producers race for one key.

**Freshness is the host cards' convention, unchanged**: expiring, written about
every 5 s with `EX 15` (3× cadence), so key absence means the producer is gone.
`ts` rides along so the card can label a column `STALE · n s` from the last
value it kept, which it renders hatched and never green. That is the same
reader-keeps-the-last-value move `health`'s `availability` note describes, and
it is why kpidash's stale state comes free rather than being a second
convention (WI 4062).

**Versioned v1, and the field names are pinned.** This is the rule korg:3955
set for the gratch panel: a producer change must not be able to blank the
card silently. Additive fields follow the ordinary rule. A rename, retype or
removal is `kdash:net:v2:{host}` (four segments, so a v1 reader's
exact-segment parse ignores it), never an edit in place. kpidash consumes
this repo as the `lib/kdashdata` submodule, so its slice moves the pointer
to the commit that lands this schema.

Rulings this contract makes (CD-27 has the reasoning):

- **`drops` is producer-side events**, not a panel-side diff of `lfc`. It
  holds unix seconds of each link-down in the last hour, oldest first, and
  is capped at 64. The panel restarts and would lose a diff's history, and
  two polls cannot place a drop in time.
- **No uplink-load field.** No producer can cheaply tell which of its bytes
  left the fabric, so the card's UPLINK tile shows the 1G link up/down.
- **A host with no bond (cleo) fills the same shape.** It sends `bond:
  false`, `leg: "5g"`, and `lfc` / `lfc_delta_24h` as `null`, which is
  present-and-null and never absent. Absence would be indistinguishable from
  a producer that forgot the field, and that fails `required`. `rx_errors`
  is real on cleo, from its one NIC.
- `lfc` is the **primary (10G) slave's** Link Failure Count, not a sum. The
  backup leg's count says nothing about the link the card is about.
- `reach.peers` means **at least one** peer answered. With "all", one peer
  down would paint every other column red.

**Writers**: kpidash-client on the Linux hosts and kpidash-client-win on cleo.
Each writes the key **directly**, over the connection that already writes
`kpidash:client:{host}:*`, and does not go through the wrappers. That is
kpidash-client's existing shape (it holds its own redis-py client and its own
khlenv resolution, checked on kai). kpidash-client-win's repo lives only on
cleo and was not read here, so its slice confirms the same route. The publisher conduct in [rules.md](rules.md) applies
regardless, in particular *validate the key* and *stamp `ts`*. `kdash` is
already in both wrappers' allowlists, so `kdash-pub set kdash:net:<host> …` is
the hand-test route and needs no code. **Readers**: kpidash's network card
(program korg:4068, slice 3). No C reader ships with this contract, and it is
registered ahead of its consumer as `stale` and `agentact` were. A typed
`kdash_net()` in libkdash is the consumer slice's to ask for, not this one's
to guess.

**`kdash:net:ack:{host}` — clearing a reviewed warning** (CD-27 addendum,
sprint 020, for kpidash WI 4143). A yellow column on the network card comes
from the producer's 24 h deltas, so it already clears itself 24 h after the
last event. The ack is the explicit clear: whoever has looked at the warning
writes the host's current `lfc` and `rx_errors` as a baseline, with `by`:

```sh
kdash-pub setex kdash:net:ack:kubsdb 86400 '{"lfc":7,"rx_errors":0,"by":"claude-kai"}'
# or with redis-cli on rpi53 (stamp ts yourself — redis-cli does not):
redis-cli SET kdash:net:ack:kubsdb '{"lfc":7,"rx_errors":0,"by":"ken","ts":1793080000}' EX 86400
```

**The ack is applied on the panel**, not by the producers. The panel already
holds the live record, and the effective delta is computable from v1 alone:
`min(<counter>_delta_24h, <counter> − ack.<counter>)`, or the live counter
itself if it is below the baseline (a reset since the ack). An event after the
ack therefore turns the column yellow again, and with no ack the card behaves
exactly as v1. The other choice would make every producer — cleo's Windows
client (korg:4076) included — read a second key to compute a field. An ack
never clears a red state (stale, no link, no path). `kdash:net:{host}` v1 is
unchanged; its readers' exact-segment parse skips this four-segment key, and
`ack` joins `v2` as a third segment that is never a host name. `kdash-pub
setex` stamps `ts` for free; a raw `redis-cli SET` must carry it, because the
column's "ack'd" marker shows its age and the schema requires it.

### `kdash:update:{host}:{kind}` — an update run that needs Ken (sprint 020)

korg WI 3641. A flag per host **per kind of issue**, so two issues on one host
— a held package and a pending reboot — show together and clear separately.
kpidash already renders one card per host from sub-keys this way for `stale`.
Known kinds: `ring-stopped`, `reboot-awaiting`, `held`, `gate-failed`. The
vocabulary is **open**: a reader renders an unknown kind by its `issue` line,
never drops it, because a dropped flag is a silent all-clear.

**Presence-owned, exactly as `stale` is (CD-18).** SET when the run finds the
issue, DEL when it is resolved, no TTL, no window. A payload a reader cannot
parse still shows the host as needing attention. `since` is carried across
runs by read-modify-write (`kdash-pub get`), never restamped. `issue` is the
one line the card renders. `ref` optionally names the korg node.

**Writer, not yet.** k-homelab's weekly apt run (proposal korg:3627) writes
**`kdash:stale:{host}:apt-update` today** (k-homelab WI 3640, Ken's interim
call, 2026-10-08). That feed's words are wrong for this ("stale" means a
deployer could not reach the host; this host was reached) and its `reason`
is not rendered. When k-homelab moves its writer here, it drops `apt-update`
from the stale feed **in the same change**, so no host is ever flagged twice.
**Reader**: kpidash's dedicated update-issue card (WI 3642), not yet built. No
C reader ships with this contract.

### `kdash:nas:{host}` — NAS health for the big panel (sprint 020)

korg WI 3955, for gratch's big panel on rpi53's kpidash (WI 3956). The WI wrote
`kpidash:nas:gratch`; `kpidash:*` is grandfathered and frozen (CD-3), so the
key is `kdash:nas:{host}`. **v1, field names pinned**, under CD-27's rule: a
breaking change is `kdash:nas:v2:{host}`.

**gratch is Ubuntu + ZFS now.** The WI's `/mnt/disk1` and `/mnt/cache` were
Unraid-era paths, so the usage bars are **per ZFS pool**. Root-filesystem
usage and the activity graph are not in this key at all, because
`kpidash:client:{host}:telemetry` and `dev_telemetry` already carry them once
kpidash-client runs on the host (korg:3952).

**The source is gratch-ui's root collector**, `/run/gratch-ui/privileged.json`
(korg:3945), read on gratch for this contract on 2026-10-29. The producer
re-reads that file and **never calls `smartctl` itself**, so the one place
that knows how to leave a standby IronWolf asleep stays the only one. This
table pins the fields the contract reads, so a gratch-ui change that breaks
one breaks it here, visibly, rather than blanking a bar:

| `kdash:nas` field | from `privileged.json` |
|---|---|
| `collected_ts` | `collected_at` (ISO 8601 → unix s) |
| `source_stale`, `source_version` | `stale`, `version` |
| `pools[].name`, `.health`, `.errors` | `pools[].name`, `.health`, `.error_count` |
| `pools[].used_bytes`, `.avail_bytes` | `pools[].used_bytes`, `.avail_bytes` — usable, after parity; not `alloc_bytes` |
| `pools[].scrub` | `pools[].scan` when `function` is `SCRUB`: `state` lower-cased, `end` → `end_ts`, `errors`; else `null` |
| `disks[].device`, `.model`, `.smart_passed`, `.temp_c` | same names |
| `disks[].kind` | `nvme` if `protocol` is `NVMe`, else `hdd`/`ssd` from `rotational` |
| `disks[].power_state` | `active` (and `idle`) → `active`, `standby` → `standby`, anything else → `unknown` |
| `disks[].smart_ts` | `smart_read_at` (ISO 8601 → unix s) |
| `disks[].reallocated_sectors`, `.pending_sectors`, `.percentage_used`, `.media_errors` | same names; `null` where the source omits them for that disk type |

Every disk-type-specific value is **present-and-null** where it does not apply
(an ATA disk's `percentage_used`, an NVMe's `pending_sectors`), never absent:
CD-27's rule. Dataset-level usage (`pools[].datasets`) is deliberately left
out until the panel design asks for it; adding it later is additive. A
missing or unreadable source file still publishes: `collected_ts: null`,
`source_stale: true`, empty arrays — so the panel says "collector down"
rather than going blank, which is what `collected_ts` vs `ts` is for.

**Freshness** is the host cards' convention: expiring, `EX` 3× the producer's
cadence, and `ts` for the stale label. **Writer, not yet**: kpidash-client on
the NAS host, over the connection it already has (as for `kdash:net`).
**Reader**: the gratch big panel (WI 3956). No C reader ships with this
contract.

### `kdash:weather:{loc}` — outdoor weather for a filler card (sprint 021)

korg WI 4573. Current conditions, today's sun times and a short daily
forecast for one named location. **v1, field names pinned**, under CD-27's
rule: a breaking change is `kdash:weather:v2:{loc}`.

**`{loc}` is a location token, not a host.** `home` is the only one today. It
obeys the host-token grammar (rules.md), so the three-segment parse is the same
as every other `kdash:<family>:{token}` key, and a second location later is a
second key with no contract change. Coordinates are not in the key or the
payload: they are the collector's private configuration.

**The consumer** is kpidash's weather filler in the host row's spare columns.
It draws at one to four column widths and not at all when the row is full, so
nothing in this feed is critical and the card trims to what fits — `days` is
capped at 8 and the card draws as many as it has room for.

**The payload is provider-neutral, and that is the design.** The collector
(kweather, korg project 85) asks Open-Meteo and falls back to NWS, and maps
either into this one shape:

- `cond` is a **closed lowercase set** — `clear`, `partly`, `cloudy`, `fog`,
  `drizzle`, `rain`, `sleet`, `snow`, `thunder` — that the collector
  classifies into. No provider code or text goes on the wire, so changing or
  adding a provider never reaches a dashboard. A new `cond` value is a
  breaking change (a v1 reader has no icon for it).
- `source` names the provider that answered. Diagnostic only; nothing branches
  on it, which is why it is a free string and not an enum.
- Units are pinned in the field names (`_f` Fahrenheit, `_pct` percent).
- `sun` (first light, sunrise, sunset, last light; unix seconds) is computed
  by the collector from latitude/longitude, not taken from a provider. First
  and last light are civil twilight. The reader formats them in its own
  timezone.
- `days[0]` is **always today** in the location's calendar. The collector
  publishes nothing rather than a list that starts on another day, so a reader
  never has to check.

**Present-and-null, never absent** (CD-27): `now`'s numbers when the provider
had a forecast but no usable current reading; today's `hi_f` on NWS when the
collector never saw it before NWS dropped the daytime period; the last day's
`lo_f` when a provider's list ends on a daytime period; `sun` fields on a
polar day or night. One documented difference a reader cannot see: `lo_f` is
the calendar day's minimum from Open-Meteo and the coming night's low from
NWS.

**Freshness** is reader-owned via `ts`, with no TTL: the last reading should
outlive a dead collector, greyed, rather than vanish. The suggested window is
**1 hour** against a 10-minute cadence. When every provider is down the
collector **publishes nothing**, so an ageing `ts` means exactly "no provider
answered since" — it never republishes an old reading under a new `ts`.

**Writer, not yet**: kweather's systemd user timer on kai, through `kdash-pub
set` (kweather WI 4574 installs it). **Reader**: kpidash's filler card. No C
reader ships with this contract; the card's session asks for one if it wants
it.

### `kdash:korg:pulse` — the korg backlog at a glance (sprint 023)

korg WI 4610, slice 2 of program korg:4619 (kpidash row 2). **v1, field names
pinned** (CD-28): a breaking change is `kdash:korg:v2:pulse`. The first
*built* key of CD-26's `kdash:korg:*` family, and it inherits all of CD-26:
publisher → central → libkdash → panel, and the publisher **copies korg's
figures and never re-derives them**.

**The consumers** are kpidash's korg + klams card, which shares one 1×1 cell
and reads `open`, `history`, `awaiting`, `active` and `by_project`, and a
standalone korg card, which adds `queued` and `projects`. Nobody has built the
standalone card yet. Its fields are contracted now so that building it later
needs no version bump.

- **`open` is korg's backlog**: work items in any non-terminal status (open,
  resolved, done, parked), unarchived, all projects. It is exactly today's
  `backlog` from `work_item_flow` and `total` from `list_work_items`. `history`
  is the same figure, day by day, oldest first, at most 30 entries, copied
  from `work_item_flow`'s `backlog` column. So the sparkline's last point and
  the headline number cannot disagree. The series is shorter, or empty, while
  korg's transition log does not reach back 30 days. Missing days are absent
  and never zero-filled, because 0 is a real backlog level.
- **`by_project` is capped at 24 and its overflow is named.** It is ordered
  largest first, with ties by name. A project with no open item is omitted.
  `other_open` counts every open item the list does not show: projects past
  the cap, plus items with no project. `projects − len(by_project)` is how
  many projects the cap left out. The sum of `by_project[].open` plus
  `other_open` equals `open`. That is a producer obligation, because the
  validator has no arithmetic (CD-24).
- **`awaiting`** is the length of korg's Awaiting Ken lane. It uses korg's
  own word, not kfdc's *Commander's Call*, because this feed reports a count
  and does not render kfdc's board. **`active`** counts proposals in status
  `active`. **`queued`** counts proposals in status `proposed`, which is the
  Planning queue. Parked proposals are excluded, because they are not
  startable.

**Freshness** is reader-owned and cadence-declared (CD-28). The writer
publishes `expected_refresh_s`. The reader calls `kdash_cadence_stale()` (in
`kdash_freshness.h`), which greys the record once `ts` is older than that
cadence plus a grace, by default `KDASH_CADENCE_GRACE_S`. The suggested
cadence is 60 s, the board read's cadence in CD-26.

**C reader**: `kdash_korg_pulse()` in `kdash_feed.h`. It is one GET on an
ordinary central-stem handle, parsed by `kdash_parse_korg_pulse()` in
`kdash_payload.h`. The parser keeps the first 24 `by_project` entries, which
are the largest, and the **newest** 30 `history` entries. Anything beyond
either cap is flagged `*_truncated`, so a record that breaks a cap still
draws. A malformed array entry is skipped and counted. **Writer**: korg's
`bin/korg-pulse` on kai, every 5 min (`korg-pulse.timer`, korg sprint 100),
through `kdash-pub set`.
`kdash` is already in both allowlists.

### `kdash:klams:status` — the klams memory server at a glance (sprint 023)

korg WI 4610, same slice. **v1, field names pinned** (CD-28): a breaking
change is `kdash:klams:v2:status`. This is the first key in the `kdash:klams:`
family. Its publisher reads klams's own health and stats and copies them,
which is CD-26's rule applied to a second system of record.

**The consumers**: the combined card reads `health`, `knowledge`, `scanners`
and `subsystems`. The standalone klams card adds `version`, `uptime_s`, the
queue, `backup_at` and `new_per_day`.

- **`health`** is a closed set: `ok`, `degraded` or `down`. `degraded` means
  klams answered but a subsystem did not. `down` means klams itself did not
  answer the publisher. **The publisher still writes a `down` record**, with
  every klams-sourced value null and both arrays empty. That keeps two
  failures apart that would otherwise look the same: a fresh `down` means
  klams is down, and an ageing `ts` means the publisher is.
- **`scanners`** has at most 8 entries. Each carries `last_wrote_at`,
  `last_ran_at` and `expected_interval_s`, so the card can draw its draining
  freshness bar without keeping a table of its own. **Wrote is not ran.** A
  scanner that runs every hour and finds nothing new is healthy, and one
  that has stopped running is not. `last_wrote_at` alone cannot tell them
  apart. The mock-up's kubs0 row is exactly that case: it last wrote three
  days ago and last ran 28 minutes ago (WI 4612). `last_ran_at` is
  **required and nullable**: a publisher with no source for it sends `null`,
  and the card says so instead of guessing (CD-27).
- **`subsystems`** has at most 8 entries of `{name, ok}`: postgres, qdrant,
  embed and rerank today. **`queue_depth` / `queue_capacity`** and
  **`backup_at`** are present-and-null whenever klams cannot supply them.
- **`new_per_day`**: knowledge records added per day, oldest first, at most
  14 entries. Only the standalone card draws it, and it may be empty.

**Freshness** follows the same cadence rule as the korg pulse. The suggested
cadence is 60 s.

**C reader**: `kdash_klams_status()` / `kdash_parse_klams_status()`. Nullable
counts read as `KDASH_COUNT_NULL` (-1), and nullable stamps read as 0, the
library's existing spelling for an unknown stamp. Over-long `scanners` and
`subsystems` keep the first 8, over-long `new_per_day` keeps the newest 14,
and each is flagged. **Writer**: klams-monitor on kubs0, every
60 s (its `[kdash_status]` section, klams sprint 059), through `kdash-pub set`.

### `kdash:calendar` — tagged days for the time and calendar card (sprint 025)

kdashdata WI 4663, slice 1 of program korg:4669 (kpidash row 3). **v1, field
names pinned** (CD-30): a breaking change is `kdash:calendar:v2`. The key is
provider-neutral, like `kdash:weather:{loc}`: korg holds the tagged-days list
today, but the feed describes days, not korg.

**The consumer** is kpidash's 2×1 time and calendar card. The card draws the
24-hour clock, the month grid and the weekends itself. This feed carries only
the dates that have something to say: Ken's days off, holidays and special
days.

- **Concrete dates only.** Yearly repeats, "fourth Thursday of November" and
  observed-weekday rules are the publisher's business. Every entry is one
  `YYYY-MM-DD`, and a reader does no date arithmetic beyond comparing those
  strings.
- **Dates are America/Los_Angeles**, the home's timezone. The fleet's Linux
  hosts run `Etc/UTC`, so a consumer that takes `today` from the system clock
  without the zone is wrong for eight hours of every day.
- **The window is `from`..`to`, inclusive.** `from` is the first day of the
  current month, so the visible month is always whole. `to` is at least 90
  days ahead. Inside the window, a date with no entry is affirmatively
  untagged. Outside it, a date is unknown.
- **One entry per tagged date**, sorted ascending, carrying all of that
  date's tags: `off` (required boolean, a personal day off), `holiday` and
  `special` (each an optional name of 1..31 characters), and `short` (an
  optional label of at most 12 characters for tight spaces). An absent
  string means no such tag. It is never null and never empty. `off` is
  independent of `holiday`: a holiday Ken works is `off: false`. A holiday
  on a weekend is listed on its date, and the weekday it is observed on is
  its own entry, named `… (observed)`. At most 64 entries.
- **Producer obligations the validator cannot check** (no `anyOf`, no date
  comparison, CD-24): every entry carries at least one tag, lies inside the
  window, and appears once; and `to` is not before `from`.

**Freshness** is not the cadence rule's to decide here. Tags change rarely, so
the card **keeps drawing the last good record**. An old `ts`, judged with
`kdash_cadence_stale()` against `expected_refresh_s` (3600 for an hourly
publisher), is a muted hint at most and **never a reason to blank the
calendar**. The real stale condition is a window that no longer covers today:
`kdash_calendar_covers()` false. Then the card draws the grid and weekends
untagged.

**C reader**: `kdash_calendar()` in `kdash_feed.h`. It is one GET on an
ordinary central-stem handle, parsed by `kdash_parse_calendar()` in
`kdash_payload.h`, which is in `kdash_core` (kpidash links only that). A
record with a bad or inverted window is rejected. An entry that is malformed,
carries no tag or lies outside the window is skipped and counted in
`days_skipped`. Past 64 the **first** (earliest) entries are kept and
`days_truncated` is set, because the earliest include this month.
`kdash_calendar_day()` looks one date up and returns NULL for an untagged one.
**Writer**: korg's `korg-calendar` timer on kai (korg sprint 101, `c49e2b6`;
k-homelab `repo-user-timer`), which checks every minute and writes when the
record changes and at least hourly, through `kdash-pub set`. Live since
2026-11-14.

### `kdash:agents` — the last 24 hours of agent sessions as bars (sprint 026)

kdashdata WI 4693, slice 1 of program korg:4699 (kpidash Agents card). **v1,
field names pinned** (CD-31): a breaking change is `kdash:agents:v2`.

**The consumer** is kpidash's 4×1 Agents card on row 3: a 24-hour timeline
with one lane of host-coloured bars, legs hung under the overseer they belong
to, a ship mark where a sprint merged, and yellow for a session waiting on
Ken. The card draws the axis, the colours and the now line itself. This feed
carries the bars.

- **A bar is a stretch of activity, not a session.** The publisher merges
  short gaps and splits at long ones, so one session may give several bars.
  `id` names a bar (≤ 23 host-token characters, unique in the record), and
  its only job on the wire is to be the target of `parent`.
- **Each bar** carries `host` (≤ 15 host-token characters), `name` (1..31
  characters, the project or session title, truncated by the publisher),
  `start` and `end` (unix seconds), `state`, `shipped`, `overseer`, and an
  optional `parent`.
- **`state`** is `done`, `working` or `waiting`. `waiting` means the session
  owes Ken an okay or a decision — ksitrep's `okay` and `decide` both map
  here. A session that is merely idle has no running bar; its last stretch is
  `done`.
- **`end` of a running bar is the publish time.** The card draws the now
  line from its own clock and clips every `end` to now. While the record is
  fresh, a `working` or `waiting` bar runs to now; once the record is stale
  (an old `ts` against `expected_refresh_s`, 60 for ksitrep), it stops at its
  `end`, so a dead publisher does not keep growing bars it last saw an hour
  ago. `kdash_agents_bar_end()` is that rule in one call.
- **The window is `window_s` before `ts`** (86400 for the card). Every bar
  that ended inside it is listed, so a host with no bar there was idle, not
  unknown. A bar that began before the window keeps its true `start`; the
  card clips it.
- **`parent`** names the overseer bar a leg hangs from. It is absent when the
  bar is not a leg or its overseer's bar is not in the record — never null
  or empty. `overseer` true marks an overseer's own bars.
- **At most 96 bars**, sorted by `start` ascending.
- **Producer obligations the validator cannot check** (CD-24): `start` ≤
  `end` ≤ `ts`; `end` ≥ `ts − window_s`; ids unique; a `parent` names another
  bar in the record whose `overseer` is true.

**C reader**: `kdash_agents()` in `kdash_feed.h`. It is one GET on an ordinary
central-stem handle, parsed by `kdash_parse_agents()` in `kdash_payload.h`,
which is in `kdash_core` (kpidash links only that). A record without a valid
window or cadence is rejected. A bar that is malformed, has an unknown
`state`, ends before it starts, ended before the window, or repeats an
earlier `id` is skipped and counted in `bars_skipped`. Past 96 the **last**
(newest) bars are kept and `bars_truncated` is set, because the card's right
edge is now. A `parent` that does not resolve to another kept overseer bar is
dropped and counted in `parents_dropped`, and its bar is kept;
`parent_index` gives the resolved overseer's slot. `name` is bounded in
characters, as the schema counts, and held in a 128-byte buffer. The record
is about 21 KB in C, so a card holds it statically. **Writer**: ksitrep on kai
(korg:4697, not yet built), every 60 s, through
`kdash-pub set kdash:agents '<json>'`. `kdash` is already in both allowlists.

### Provisional: `kdash:korg:*` — korg data for the panels (CD-26, sprint 018)

**Decided, not built.** Neither key below is written yet, and neither has a
schema or a reader. The rows exist so that the next person to build a korg
widget finds the decision before reaching for korg's HTTP API. A panel
**never** calls korg directly (CD-26 rejects that option on the record). The
family's first *built* key is `kdash:korg:pulse`, above.

| Key | Type / pattern | Writer (cadence) | TTL | Schema |
|---|---|---|---|---|
| `kdash:korg:board` | latest-value, ts-owned | korg-side publisher reading `GET /api/board` (default 60 s) | none | — provisional, lands with the writer |
| `kdash:korg:rate_of_fire` | latest-value, ts-owned | the same publisher reading `GET /api/work-items/flow` (default 5 min) | none | — provisional, lands with the writer |

The vocabulary is kfdc's (Fire Missions, On Deck, Commander's Call,
Operations, Rate of Fire). kstudiodash's *Awaiting Ken* (the consumer CD-26
was written for, retired with kstudio on 2026-10-20) was a rendering of
`board`'s `commanders_call`. The publisher copies korg's computations and never
re-derives them. Its host, repo and cadence are stated defaults in CD-26,
settled when it is planned.

## Family: ghcp (central, live)

Owner: kdeskdash (`publisher/ghcp-pub.sh`, shipped in the same package-store
bundle as `claude-pub.sh`); installed by k-homelab's `copilot-hooks` recipe on
kai, kubs0 and komarchy, and by hand on cleo, exactly as the Claude hooks are.
Endpoint: `KDASH_CLAUDE_REDIS`, answering `rpi53:6379` — the same home as
`claude:*`, because the panels that read one read the other in the same pass.

| Key | Type / pattern | Schema | Notes |
|---|---|---|---|
| `ghcp:session:{host}:{sid}` | HASH, TTL 7200 s | [session](schemas/ghcp-session.schema.json) | field-for-field `claude:session`'s shape; `{sid}` is Copilot's own `sessionId` uuid; sessionEnd DELs the key |

**Not grandfathered — new, and frozen from day one.** `ghcp:` is
grandfathered-*shaped*: it sits outside the `kdash:<family>:` namespace
rules.md reserves for new shared feeds. That is a deliberate exception rather
than a slip. kxeneon's reader and the sprint-004 ruling already spell it this
way, and renaming a family to satisfy a naming rule — before it has a single
writer — would spend the one thing the rule exists to protect. The consequence
travels with the exception: because it is new, there is nothing to migrate
opportunistically and no historical shape to honour, so a change to it is a
**versioning event** under rules.md, not a migration.

**The field set is `claude:session`'s on purpose** (CD-21). Same names, same
meanings, same required pair, same resurrection-race guard. Copilot is not
Claude and a field set designed for it alone would be a better description of
Copilot — and would double every consumer's parsing and display code for the
privilege. kxeneon's `parse_session`, libkdash's derivation and CD-16's
attention ladder apply to both families unchanged, and a session row renders
identically whichever agent produced it. Anything Copilot has that Claude does
not can arrive later as an added field; the additive rule already allows it.

**What a Copilot hook cannot say, measured.** The user-level hook set was
probed live on kai against Copilot CLI 1.0.83 in sprint 012 — a temporary hook
file declaring every candidate event, two real sessions, and the stdin of each
event captured. The complete set is `sessionStart`, `sessionEnd`,
`userPromptSubmitted`, `preToolUse`, `postToolUse`, `errorOccurred`; six
speculative names (`turnEnd`, `responseCompleted`, `stop`, `assistantMessage`,
`notification`, `preCompact`) fired nothing and drew no warning, so **a
mistyped event name in a hook file fails silently** — worth knowing for the
recipe that installs one. Every payload carries `sessionId`, `timestamp` and
`cwd`; `sessionStart` adds `source` and `initialPrompt`, the tool events add
`toolName`/`toolArgs` (and `postToolUse` a `toolResult`), and `sessionEnd`
adds `reason`.

Three consequences the schema states and a reader must hold:

- **There is no turn-end event.** Nothing fires when the agent finishes
  replying. So a publisher can raise `working` and can DEL on `sessionEnd`, and
  can never observe a session becoming `awaiting`. A Copilot session that is
  really waiting for its user keeps saying `working` until the reader's
  freshness ladder ages it (idle at 15 min, stale at 40). That window is the
  feed's known blind spot and the reason the ladder is not optional here.
  `blocked` has no event either — `preToolUse` fires before a *tool*, not
  before a permission prompt, and the probe ran `--allow-all-tools`, so the
  approval path is recorded as unobserved rather than as absent.
- **`timestamp` is in milliseconds.** `ts` is unix seconds everywhere in this
  repo, so the publisher divides by 1000. Passing it through would put every
  record a thousand-fold into the future, and since readers treat negative ages
  as clock skew and therefore fresh, a session that ended weeks ago would show
  as live forever with nothing reporting an error anywhere. **The schema bounds
  `ts` and `started_ts` at 1e11** — the year 5138 in seconds, which every
  millisecond stamp after 1973 exceeds — so the slip is a validation failure
  rather than a convincing record. That bound was added because a negative test
  caught the prose version letting one straight through: this family's whole
  documented hazard, and the documentation did not stop it. `claude:session`'s
  `ts` carries no such bound, which is the one place the two families
  deliberately differ — and that is **settled**, not pending: Ken ruled on
  2026-09-16 that it stays unbounded, per CD-3. A grandfathered shape is
  documented as it is, never tightened underneath a writer this repo does not
  own; the bound belongs to new families, where it catches a new writer's unit
  slip before it publishes.
- **`userPromptSubmitted` fires *before* `sessionStart`** — reproducibly, by
  about 4 ms, in `-p` mode. A publisher that treats `sessionStart` as "the
  first write" clobbers a record that already exists; taking `started_ts` from
  the payload's own stamp is correct in either order.

`model` and `title` are consequently **normally absent**: no hook event carries
either. The claude family fills them from the transcript; Copilot's equivalent
state sits in `~/.copilot/session-state/<id>/`, which a hook could read but
which no hook hands it. Both fall back to `project` in the view, which costs
the consumer nothing because that fallback already exists.

**No usage feed, and no `ghcp:limits`.** The sprint-004 ruling stands:
Microsoft-managed quota numbers carry no signal that changes on a dashboard's
timescale. A `ghcp:recent` remains optional and unschema'd until somebody wants
it — `sessionEnd` carries the `reason` it would need (`complete` on a clean
exit), so it is one extra `LPUSH` in the same batch on the day it is asked for.

**Readers**: kxeneon's Agents panel, which already reads this key pattern and
whose field guesses this contract replaces. kdeskdash's claude mode may follow.
No C reader ships with this contract.

## Family: claude (central, live)

Owner: the Claude-activity publisher (`publisher/claude-pub.sh` + Claude Code
hooks/statusline; see kdeskdash sprint 007). Written from every fleet host that
runs Claude Code (kai, kubs0, cleo); read by kdeskdash (rpidash2 + rpidash3).
Shapes are grandfathered (CD-3) but schema'd as of the relocation — CD-7 said
the schemas land with the move, and they did.

Endpoint: `KDASH_CLAUDE_REDIS`, answering `rpi53:6379`. The publisher reaches it
through `kdash-pub` (CD-11/CD-13) — khlenv discovery, CD-12 auth and the key
grammar, with no hardcoded address anywhere on the write path.

| Key | Type / pattern | Schema | Notes |
|---|---|---|---|
| `claude:session:{host}:{sid}` | HASH, TTL 7200 s | [session](schemas/claude-session.schema.json) | host/project/cwd/status/ts/started_ts + model/title; `status` ∈ working/awaiting/blocked; records missing `status` or numeric `ts` are rejected (resurrection-race guard) |
| `claude:limits` | HASH, no TTL | [limits](schemas/claude-limits.schema.json) | five_hour/seven_day pcts + resets; `five_hour_none` 1 when the source reports no five-hour window (CD-29); writers publish their own cadence (`expected_refresh_s`); the model-scoped window carries its own independent stamp |
| `claude:recent` | event log, capped: LPUSH + LTRIM 0 19 | [recent](schemas/claude-recent.schema.json) | `{host, project, title, ended_ts, dur_s}` |

The two HASH feeds are the only records in this registry that are **not** JSON
documents. Their schemas describe the decoded record; every value arrives off
the wire as a string.

**C readers** (sprint 006, [architecture](../docs/architecture.md) CD-15/CD-16):
`kdash_claude_sessions()`, `kdash_claude_limits()` and `kdash_claude_recent()`
in `include/kdash/kdash_feed.h` — SCAN + HGETALL, HGETALL, and LRANGE
respectively, on a handle opened at `&KDASH_STEM_CLAUDE`. The HASH pair parses
from an HGETALL field/value list rather than a buffer (CD-15), which is the one
parser shape the pure core did not already have. The schemas above stay the
source of truth; the C structs are their projection.

Freshness ladder (reader-derived, the CD-6 model): published status trusted
while fresh; no event for 15 min → idle; 40 min → stale. Derived in the library
(`kdash_claude_sessions_refresh()`), which also orders sessions attention-first;
labels and time formatting stay with the panel (CD-16).

`claude:limits` is one shared key with several writers on several hosts, so
`updated_at` is the *observation* time and a writer must not publish over a
fresher one. That guard is a read, which is why the publisher CLI has exactly
one read verb (CD-14).

**No five-hour window** (sprint 024, CD-29). Some plans no longer have a
five-hour window, and the oauth endpoint then returns `"five_hour": null`.
`five_hour_pct` stays required, so the record says it with a flag. Every
headline writer puts it in the same HSET as `updated_at`:

| the source shows | the writer writes | a C reader (`kdash_claude_limits_t`) sees |
|---|---|---|
| a five-hour window | `five_hour_none 0`, real `five_hour_pct`, `five_hour_resets_at` if known | `valid`, `!five_hour_none`, the values as published |
| no five-hour window | `five_hour_none 1`, `five_hour_pct 0`, `five_hour_resets_at 0` | `valid`, `five_hour_none`, both five-hour fields 0 whatever the hash holds |
| (a writer from before this field) | no `five_hour_none` | `valid`, `!five_hour_none`; its `five_hour_pct` is a reading |
| nothing usable | — | `KDASH_ABSENT` / `KDASH_UNAVAIL`, or `!valid`; the struct is zeroed, so `five_hour_none` is false. Unknown is never "no window" |

**The writer that observes no window clears the leftovers.** Writing 0 rather
than leaving the reset alone is deliberate. **A writer that observes a window
clears the flag.** The reader zeroes a leftover five-hour value beside
`five_hour_none 1` anyway. The flag rides `updated_at`, so it goes stale with
the rest of the headline.

**Relocated from `rpidash2:6380`** by program korg:1755, in four slices:
publisher wrappers and the stem (sprint 003), `kdash-pub` distribution
(sprint 004), the kdeskdash cutover and reader repoint through a dual-write
window (kdeskdash sprint 031), and this close-out (sprint 005). The old home
still serves `kvscf:*` for the dev pair and always will (CD-8) — this family
retired a *feed* from that instance, not the instance.

## Family: kvscf (live, two homes — dev pair on central, work pair with its pair)

Owner: the Windows publisher on the pair's workstation — kctrldeck on cleo for
the dev pair, kvscf on kwork for the work pair. Read and commanded by the desk
dashboard in front of that keyboard: a direct data + control exchange within
one pair. Read `kvscf` in this family's names as "whichever app publishes on
that host"; the namespace stays put (korg WI 2479) because renaming it would
touch every consumer for no gain.

**Two homes since sprint 014** (CD-8 as amended, program korg:2935):

| pair | home | auth |
|---|---|---|
| cleo ↔ rpidash2 (dev) | **central, `rpi53:6379`** | the fleet `REDISCLI_AUTH` |
| kwork ↔ rpidash3 (work) | `rpidash3:6380` | that instance's own password |

The work pair does not move and is not expected to: kwork is LAN-only and
MS-managed, so it cannot reach central and must not be given the fleet
password. Both panels pin `KDESKDASH_KVSCF_REDIS_HOST/PORT` explicitly
(kdeskdash 031), which is what lets one panel be repointed without the other;
the auth still inherits from the claude values, but **only** when the two
resolve to the same `host:port`, because sending a password to a Redis that has
none configured is an error rather than a shrug.

The shapes are unchanged and stay grandfathered — this is a change of home for
one pair's half, not a contract change:

| Key / channel | Type / pattern | Notes |
|---|---|---|
| `kvscf:instances:{host}` | latest-value (JSON, large) | open VS Code windows on that host |
| `kvscf:edge:{host}`, `kvscf:apps:{host}`, `kvscf:launcher:{host}` | latest-value | launcher/app surfaces; a launcher button carries a stable `key`, never a command line |
| `kvscf:focus:{host}` | PUB/SUB nudge | focus command; payload carries an auth token — the one nudge-pattern feed in the fleet |

Two consequences of the dev pair's move, both on the consumer's side and both
written down where the consumer slice will read them (CD-8):

- **The host segment is now the only scoping.** Panels discover with `SCAN
  kvscf:instances:*` and friends, which returned one workstation's keys because
  only one workstation wrote to that instance. On central a panel must take its
  pair host from **configuration, not discovery**.
- **The token on `kvscf:focus:{host}` is now readable by every holder of the
  fleet password.** Accepted with the move, because keeping the channel off
  central would mean keeping the server the program exists to retire; the
  answer is the rotatable per-host pairing token (korg WI 2479) and OQ-2, not
  the topology (CD-23).

## Dashboard-local namespaces (visibility only, not governed)

| Namespace | Status | Host | Keys today |
|---|---|---|---|
| `kdeskdash:*` | migrating | each kdeskdash device | `active_mode`, `screenshot`, `gol:settings`, `golz:{wins,human_wins,zombie_wins,ties,gens_to_win,settings}`, `dev:left`, `dev:right` — **being retired** by program korg:2935: the durable half moves to a file on the panel, the control half to `kdash:panelmode` / `kdash:panelshot` above |
| `kstudiodash:*` | retired | kstudio | nothing, ever — kstudio ran no local Redis (which is why its panel-control key was `kdash:panel:kstudio` on central). Retired with the host on 2026-10-20 (WI 3986); both publisher allowlists refuse it, and `check-docs` reads this column to know they should |

## Reserved

- `kdash:<family>:<…>` — the namespace for new shared feeds (rules.md).
  `selftest`, `panel`, `panelmode`, `panelshot`, `stale`, `agentact`, `net`,
  `update`, `nas`, `weather`, `korg` and `klams` are the families in it so far
  (`korg` per CD-26, contracted with `klams` by CD-28).
- `ghcp:*` — outside that namespace by a named exception (CD-21), not by
  omission. New, frozen from day one, and not a migration candidate.
