# Sprint 001 — data contract v0

korg: proposal 1733, WI 1739. Goal: document the Redis feed reality the
three LVGL dashboards live in today, define the rules new feeds follow, and
record the standing decisions — docs and schemas only, zero code.

## What shipped

- **[docs/architecture.md](../docs/architecture.md)** — topology and the six
  standing decisions (CD-1…CD-6): central-Redis-plus-local topology, one
  fleet password with `REDISCLI_AUTH` as the auth contract, no big-bang
  renames, khlenv discovery (new stem `KDASH_CENTRAL_REDIS`, `KPIDASH_REDIS`
  kept as legacy alias), read-only consumers, degrade-don't-block. Two open
  questions: OQ-1 (home of the `claude:*`/`kvscf:*` families — leaning
  migrate to central), OQ-2 (ACL writer/reader split).
- **[contracts/rules.md](../contracts/rules.md)** — the normative half for
  new feeds: `kdash:*` namespace, key grammar (host-token contract,
  fixed segment counts), the five feed patterns (expiring latest-value,
  ts-owned latest-value, capped event log, GETDEL one-shot, pub/sub nudge),
  payload rules (`ts` everywhere, readers ignore unknown fields),
  schema-per-feed, and versioning (breaking = new key segment, never
  mutate in place).
- **[contracts/registry.md](../contracts/registry.md)** — the v0 inventory:
  every key family across the three homes (central `rpi53:6379`,
  `rpidash2:6380` for claude/kvscf, dashboard-local), writer, pattern, TTL,
  and freshness semantics, grandfathered as-is per CD-3.
- **[contracts/schemas/](../contracts/schemas/)** — JSON Schemas
  (draft 2020-12) for the five cross-dashboard kpidash payloads: health,
  telemetry, dev_telemetry, service_status, apttemps.

## How the reality was established

Mined the working implementations rather than guessing: kpidash's
`docs/CLIENT-PROTOCOL.md` v2.0 (the pre-existing canonical reference — v0
largely centralizes and generalizes it), `kpidash-client`'s
`redis_client.py`/`endpoint.py` (key writes, TTLs, khlenv resolution
semantics), and kdeskdash's `config.c`, `claude_feed.h`, `claude_redis.c`,
`kvscf_redis.c` (consumer endpoints, the claude/kvscf families, the
freshness ladder). The klams record of the 2026-08-15 red-card faults
supplied the publisher matrix and the "red card means nobody is publishing"
framing.

## Decisions made in-sprint

- The five schema'd feeds are the ones a second dashboard would plausibly
  consume; kpidash-internal families (activities, repos, fortune, cmd,
  system) are documented but not schema'd — they migrate when touched.
- The `claude:*`/`kvscf:*` families are deliberately **not** schema'd yet:
  their home is OQ-1, and schema'ing before the home is settled would
  freeze the wrong thing. Gate: settle OQ-1 before kstudiodash consumes
  either family.
- `kdash:*` reserved for new shared feeds; nothing lands there until a real
  feed needs it.

## Follow-ups

- Sprint 002: the shared C consumer library (CD-6 behaviors, the choke-point
  validation, aarch64 + x86_64).
- OQ-1 decision — blocks the claude/kvscf schemas and affects kdeskdash env
  defaults.
- kstudiodash 005 (korg:1728) is gated on kdashdata being consumable
  (contract + C lib) and serves as the live verification.
- Known divergence to fix at the source: the Windows kpidash-client takes
  its password from an inline config field instead of `REDISCLI_AUTH`
  (kpidashclient-win #1294; CD-2 names REDISCLI_AUTH as the contract).

Note for the ship: this repo is public and these docs name internal
hostnames (rpi53, rpidash2, kstudio) — consistent with the project's
public-from-day-one stance and free of secrets/endpoints beyond hostnames,
but it is a deliberate call, not an oversight.

## Addendum (same day, sprint review)

OQ-1 settled on review with Ken and split into two decisions: **CD-7** —
`claude:*` migrates to the central Redis, executed as a korg **program**
because the blast radius (fleet-wide publisher hooks, both kdeskdash
devices, AUTH coverage, cutover freshness) exceeds a single sprint;
`rpidash2:6380` is the sanctioned interim home until then. **CD-8** —
`kvscf:*` never moves: its data is symbiotic between kvscf on a workstation
(cleo/kwork) and the desk dashboard in front of that keyboard — a pair-local
data + control exchange, not fleet-shared state.
