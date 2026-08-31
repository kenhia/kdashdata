# Feed registry — contract v0 (current reality)

> The inventory of every Redis feed the dashboards touch, as it exists
> today. Shapes here are **grandfathered** (CD-3): documented as-is, no
> renames. New feeds follow [rules.md](rules.md). Family status is `live`,
> `migrating`, or `retired`.

## Homes

| Home | Endpoint | Auth | Holds |
|---|---|---|---|
| Central | `rpi53:6379` | `REDISCLI_AUTH` (krot: rpi53-redis-password) | `kpidash:*` |
| Claude/kvscf (OQ-1) | `rpidash2:6380` | per-app env | `claude:*`, `kvscf:*` |
| Dashboard-local | `127.0.0.1:6379` on each dashboard host | none/local | `<dashboard>:*` |

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

## Family: claude (rpidash2:6380, live — home under OQ-1)

Owner: the Claude-activity publisher (`publisher/claude-pub.sh` + Claude
Code hooks/statusline; see kdeskdash sprint 007). Read by kdeskdash
(rpidash2 + rpidash3). Not yet schema'd — do that when the family's home
(OQ-1) is settled or when kstudiodash consumes it, whichever comes first.

| Key | Type / pattern | Notes |
|---|---|---|
| `claude:session:{host}:{sid}` | HASH | host/project/cwd/status/ts/started_ts + model/title; `status` ∈ working/awaiting/blocked; records missing `status` or numeric `ts` are rejected (resurrection-race guard) |
| `claude:limits` | HASH | five_hour/seven_day pcts + resets; writers publish their own cadence (`expected_refresh_s`); model-scoped window carries its own stamp |
| `claude:recent` | capped LIST of JSON | `{host, project, title, ended_ts, dur_s}` |

Freshness ladder (reader-derived, the CD-6 model): published status trusted
while fresh; no event for 15 min → idle; 40 min → stale.

## Family: kvscf (rpidash2:6380 by convention, live — home under OQ-1)

Owner: kvscf (Windows publisher on cleo). Read/commanded by kdeskdash.
kdeskdash defaults this endpoint to the claude endpoint; rpidash3
deliberately points at a different (work-side) kvscf instance.

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

- `kdash:*` — the namespace for new shared feeds (rules.md). No keys exist
  yet; the first will land with the first post-v0 feed.
