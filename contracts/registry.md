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
`services` keys are exactly 4 segments, `_` = "no host"; non-conforming keys
are ignored. `services`/`apttemps` staleness is reader-owned via `ts`
(60 s / 300 s windows in kpidash today). A red card means "nobody is
publishing", not "that host is down" — check the publisher.

**Bands (`apttemps`).** What counts as cold / ok / warm / hot is part of the
shared data model, not each panel's taste: `kdash_apttemps_band()` in
[`kdash_payload.h`](../include/kdash/kdash_payload.h), with the thresholds
`KDASH_APTTEMPS_{COLD,OK,HOT}_F` in
[`kdash_freshness.h`](../include/kdash/kdash_freshness.h) as defaults. Below
65 F cold, 65–75 ok, 75.1–79.9 warm, 80 and above hot — kpidash's bands,
which kstudiodash had ported by hand. Which *colour* a band renders as stays
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
| `kpidash:activities`, `kpidash:activity:{uuid}` | event log: ZSET index + HASH per entry, capped 20 | activity ticker |
| `kpidash:repos:{host}` | HASH field=path value=JSON, ~30 s | only non-clean/off-default repos are written; clean → `HDEL` |
| `kpidash:fortune:current`, `kpidash:fortune:pushed` | latest-value | pushed overrides rotation, EX 300 |
| `kpidash:status:current`, `kpidash:status:ack:{id}` | latest-value + ack key (EX 60) | the CD-5 sanctioned client-write example |
| `kpidash:cmd:*` | dev/control toggles, EX 300 | grid, textsize, graph, fortune_dev, services:evict (GETDEL) |
| `kpidash:screenshot` | one-shot command (GETDEL) | device self-screenshot |
| `kpidash:system:*` | diagnostics | logpath/version plain strings; mem:current + mem:ring (LPUSH, trim 1500) |

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

`selftest` is a publish canary, not a dashboard feed: running it from a host
proves that host's whole publish path — khlenv discovery, `REDISCLI_AUTH`, key
grammar, schema-valid payload — and nothing renders the result. Key absence
means nobody has run it there lately, which is not a fault.

`panel` is the family's first **control** feed (CD-17): it tells the dashboard
running on `{host}` which screen to show — `dash` or `desktop` — and
kstudiodash acts on it by switching VT. Deliberately *state* rather than the
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
kdash-pub set kdash:panel:kstudio '{"want":"desktop"}'
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
| `kdash:panel:{host}` | show the dashboard, or yield the display (`want`: `dash` \| `desktop`) | kstudiodash today |
| `kdash:panelmode:{host}` | show this screen, optionally configured like this | kdeskdash today |
| `kdash:panelshot:{host}` | capture the screen, optionally to this path | kdeskdash today |

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

The wrappers validate the namespace and the token charset, **not** this
family's segment count — `kdash:stale:komarchy` with the deployer left off
publishes without complaint and no reader will ever look at it. Arity is the
reader's choke point by design ([rules.md](rules.md), Key grammar), so a
deployer builds this key carefully or not at all.

**Readers**: kpidash renders a `<host> stale` card while any key for that host
exists, listing the deployers behind it; kmon may later read it instead of
counting an intermittent host against fleet health. No C reader ships with this
contract — the feed is registered ahead of its consumers on purpose. The day a
dashboard wants it through libkdash it needs a **SCAN over
`kdash:stale:{host}:*`** and a parse (both identity segments come off the key),
not the single GET the other two `kdash:*` feeds use.

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
  deliberately differ (see the follow-up on that family).
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
| `claude:limits` | HASH, no TTL | [limits](schemas/claude-limits.schema.json) | five_hour/seven_day pcts + resets; writers publish their own cadence (`expected_refresh_s`); the model-scoped window carries its own independent stamp |
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

| Namespace | Host | Keys today |
|---|---|---|
| `kdeskdash:*` | each kdeskdash device | `active_mode`, `screenshot`, `gol:settings`, `golz:{wins,human_wins,zombie_wins,ties,gens_to_win,settings}`, `dev:left`, `dev:right` — **being retired** by program korg:2935: the durable half moves to a file on the panel, the control half to `kdash:panelmode` / `kdash:panelshot` above |
| `kstudiodash:*` | kstudio | reserved — still nothing. kstudio runs no local Redis, which is why the panel-control feed is `kdash:panel:kstudio` on **central** and not a local key here |

## Reserved

- `kdash:<family>:<…>` — the namespace for new shared feeds (rules.md).
  `selftest`, `panel`, `panelmode`, `panelshot`, `stale` and `agentact` are
  the families in it so far.
- `ghcp:*` — outside that namespace by a named exception (CD-21), not by
  omission. New, frozen from day one, and not a migration candidate.
