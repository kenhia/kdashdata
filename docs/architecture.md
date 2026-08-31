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
the **shared C consumer library** the dashboards link (sprint 002+), and thin
**Rust/Python publisher wrappers** (later). Nothing in this repo runs as a
service.

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

Exception today: the Claude-activity and kvscf feed families live on
`rpidash2:6380`, not the central Redis — see OQ-1.

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
- **Local Redis needs no discovery** — it is `127.0.0.1:6379` by definition;
  per-app env overrides (e.g. `KDESKDASH_REDIS_HOST`) stay as-is.
- Feed families not on the central Redis (see OQ-1) keep their per-app env
  configuration until their home is settled.

Resolution rules inherited from the kpidash-client implementation: resolve
on **every** connect (a moved Redis is picked up within one loop interval);
local config override wins outright; khlenv never holds the password (CD-2).

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

## Open questions

- **OQ-1 — Home of the `claude:*` and `kvscf:*` families.** Today they live
  on `rpidash2:6380` (rpidash2's local instance doubles as the shared home;
  rpidash3 reads it over the LAN). Options: migrate to the central Redis
  (leaning — one shared home beats two; requires REDISCLI_AUTH coverage on
  the writers and a kdeskdash env change) or sanction `rpidash2:6380` as a
  second shared home permanently. Decide before kstudiodash consumes either
  family.
- **OQ-2 — Redis ACL writer/reader split** (see CD-2).
