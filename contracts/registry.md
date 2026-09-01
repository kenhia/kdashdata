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
| Claude interim (CD-7) | `KDASH_CLAUDE_REDIS` | `rpidash2:6380` | **none configured** — sending AUTH here is an error, so publishers need the explicit no-auth opt-out (CD-12); central requires AUTH, so the cutover reverses that | `claude:*` — until the migration program runs |
| Workstation pair (CD-8) | per-app env (no stem yet) | dev pair `rpidash2:6380`; work pair its own | per-app env | `kvscf:*` — stays with the pair by design |
| Dashboard-local | none — `127.0.0.1:6379` by definition | `127.0.0.1:6379` on each dashboard host | none/local | `<dashboard>:*` |

`KDASH_CLAUDE_REDIS` is the one the relocation program flips (CD-7): it names
the interim home today and the central Redis after the cutover, which is what
makes that cutover a store edit rather than a sweep of every publisher host.
`kvscf:*` deliberately has **no** stem yet — kdeskdash currently defaults its
kvscf endpoint to the claude one, and CD-8 requires that pin to be made
explicit as part of the flip, in the kdeskdash slice that owns it.

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

## Family: claude (rpidash2:6380 interim, live — migrating to central per CD-7)

Owner: the Claude-activity publisher (`publisher/claude-pub.sh` + Claude
Code hooks/statusline; see kdeskdash sprint 007). Read by kdeskdash
(rpidash2 + rpidash3). Will move to the central Redis via a korg program
(CD-7 — blast radius: every fleet host's hooks, both kdeskdash devices,
AUTH coverage, cutover freshness). Not yet schema'd — schemas land with the
move program, or sooner if a new consumer (kstudiodash) needs them first.

Endpoint: `KDASH_CLAUDE_REDIS`, seeded in sprint 003 and answering
`rpidash2:6380`. Today's publisher does not use it — `claude-pub.sh` carries a
hardcoded `192.168.1.144:6380` and speaks RESP over `/dev/tcp` with no AUTH —
and replacing that with `kdash-pub` is the cutover the program's kdeskdash
slice performs.

| Key | Type / pattern | Notes |
|---|---|---|
| `claude:session:{host}:{sid}` | HASH | host/project/cwd/status/ts/started_ts + model/title; `status` ∈ working/awaiting/blocked; records missing `status` or numeric `ts` are rejected (resurrection-race guard) |
| `claude:limits` | HASH | five_hour/seven_day pcts + resets; writers publish their own cadence (`expected_refresh_s`); model-scoped window carries its own stamp |
| `claude:recent` | capped LIST of JSON | `{host, project, title, ended_ts, dur_s}` |

Freshness ladder (reader-derived, the CD-6 model): published status trusted
while fresh; no event for 15 min → idle; 40 min → stale.

## Family: kvscf (workstation-pair Redis, live — stays put per CD-8)

Owner: kvscf (Windows publisher on the pair's workstation: cleo for the dev
pair, kwork for the work pair). Read/commanded by the desk dashboard in
front of that keyboard — a direct data + control exchange within one pair,
which is why this family never moves to central (CD-8). kdeskdash defaults
this endpoint to the claude endpoint today (dev pair `rpidash2:6380`);
rpidash3 points at the work-side instance. Once `claude:*` migrates (CD-7),
kvscf keeps the pair endpoint — the shared default decouples then.

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
