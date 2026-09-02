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
| Claude feed (CD-7, relocated) | `KDASH_CLAUDE_REDIS` | `rpi53:6379` | `REDISCLI_AUTH` (krot: rpi53-redis-password) | `claude:*` |
| Workstation pair (CD-8) | per-app env (no stem yet) | dev pair `rpidash2:6380`; work pair its own | per-app env | `kvscf:*` — stays with the pair by design |
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

`kvscf:*` deliberately has **no** stem, and no longer needs one to stay put:
both panels pin `KDESKDASH_KVSCF_REDIS_HOST/PORT` explicitly since kdeskdash
sprint 031, so CD-8 holds by its own configuration rather than by riding on
wherever the claude endpoint happens to point. That pin is not a formality —
the credential inherited across a differing endpoint is what actually broke
during the repoint (see the claude family below).

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

`selftest` is a publish canary, not a dashboard feed: running it from a host
proves that host's whole publish path — khlenv discovery, `REDISCLI_AUTH`, key
grammar, schema-valid payload — and nothing renders the result. Key absence
means nobody has run it there lately, which is not a fault.

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

## Family: kvscf (workstation-pair Redis, live — stays put per CD-8)

Owner: kvscf (Windows publisher on the pair's workstation: cleo for the dev
pair, kwork for the work pair). Read/commanded by the desk dashboard in
front of that keyboard — a direct data + control exchange within one pair,
which is why this family never moves to central (CD-8). Both panels now pin
`KDESKDASH_KVSCF_REDIS_HOST/PORT` explicitly (rpidash2 at its own
`127.0.0.1:6380`, rpidash3 at its own second instance); the endpoint used to
default to the claude one, and the auth still inherits — but only when the two
resolve to the same `host:port`, because sending a password to a Redis that has
none configured is an error rather than a shrug. Now that `claude:*` has moved
(CD-7),
kvscf keeps the pair endpoint — the shared default decoupled at that flip.

| Key / channel | Type / pattern | Notes |
|---|---|---|
| `kvscf:instances:{host}` | latest-value (JSON, large) | open VS Code windows on that host |
| `kvscf:edge:{…}`, `kvscf:apps:{…}`, `kvscf:launcher:{…}` | latest-value | launcher/app surfaces |
| `kvscf:focus:{host}` | PUB/SUB nudge | focus command; payload carries an auth token — the one nudge-pattern feed in the fleet |

## Dashboard-local namespaces (visibility only, not governed)

| Namespace | Host | Keys today |
|---|---|---|
| `kdeskdash:*` | each kdeskdash device | `active_mode`, `screenshot`, `gol:settings`, `golz:{wins,human_wins,zombie_wins,ties,gens_to_win,settings}`, `dev:left`, `dev:right` |
| `kstudiodash:*` | kstudio | reserved — nothing yet |

## Reserved

- `kdash:<family>:<…>` — the namespace for new shared feeds (rules.md).
  `selftest` is the only family in it so far.
