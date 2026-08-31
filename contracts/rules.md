# Feed contract rules

> Contract v0. These rules bind **new** feeds; existing families are
> grandfathered as documented in the [registry](registry.md) (CD-3). The
> current inventory lives there; the topology and decisions live in
> [architecture](../docs/architecture.md).

## Namespaces

- **New shared feeds**: `kdash:<family>:<…>` on the central Redis. One
  family = one owner (the publisher project) = one schema set here.
- **Dashboard-local state**: `<dashboard>:<…>` (e.g. `kdeskdash:*`,
  `kstudiodash:*`) on that dashboard's local Redis. Local state is that
  dashboard's business; it is listed in the registry for visibility but not
  schema-governed.
- **Grandfathered**: `kpidash:*`, `claude:*`, `kvscf:*` — frozen shapes,
  documented in the registry, migrated opportunistically.

## Key grammar

- Lowercase, `:`-separated segments; no spaces.
- Host and session tokens: `[A-Za-z0-9._-]`, 1–63 chars, matching lowercase
  `hostname` output. Readers validate at a single choke point and **skip**
  any key that fails the contract (never crash, never guess).
- A family's keys have a **fixed segment count**. Readers parse by exact
  segment count and silently ignore non-conforming keys — this is what makes
  additive evolution safe (learned the hard way in kpidash's services
  family; "no host" is spelled with a sentinel segment `_`, not a missing
  segment).

## Feed patterns — pick one per feed

| Pattern | Redis shape | Freshness | Use when |
|---|---|---|---|
| **Latest-value, expiring** | STRING (JSON), `SET … EX <ttl>` | Key absence = source offline | The value's *presence* is the liveness signal (health pings) |
| **Latest-value, ts-owned** | STRING (JSON), no TTL, `ts` field required | Reader compares `now - ts` to the family's staleness window | Value should outlive its writer (status cards); reader owns the staleness policy |
| **Event log, capped** | ZSET index + HASH per entry, or LPUSH + LTRIM list | Per-entry timestamps | Append-shaped history (activities, recents). Writer owns the cap |
| **One-shot command** | STRING, consumed with `GETDEL` | Consumed = handled | Trigger with exactly-once handling (screenshot, evict) |
| **Nudge** | PUB/SUB channel | None — fire and forget | Latency-sensitive pokes where a lost message is harmless. **Never state** |

TTL guidance for expiring values: TTL ≈ 3× write cadence — tight enough that
staleness means something, loose enough that one missed write isn't a flap.

## Payloads

- JSON objects, `snake_case` field names, UTF-8.
- Every payload carries `ts` (unix seconds, float) — even when the key also
  has a TTL. Timestamps are the writer's clock; readers treat negative ages
  (skew) as fresh.
- **Readers ignore unknown fields; writers may add fields freely.** That is
  the additive-evolution contract, and it cuts both ways: a reader that
  rejects unknown fields is broken, and a writer that *changes or removes*
  a field is making a breaking change (see Versioning).
- Required fields are required: a payload missing one is rejected whole by
  the reader (skip the record, count it, render without it).

## Schemas

- One JSON Schema file (draft 2020-12) per feed payload, in
  [`schemas/`](schemas/). **The schema file is the source of truth**; prose
  in the registry summarizes but never overrides it.
- Schemas set `additionalProperties: true` — that is the additive-evolution
  rule expressed in schema form.
- A feed exists when its schema file lands here; a publisher writing a key
  with no schema in kdashdata is off-contract.

## Versioning

- Additive change (new optional field, new key in an existing family):
  update the schema, note it in the registry. No coordination needed.
- **Breaking change** (field removed/renamed/retyped, semantics changed):
  a **new key family segment** — `kdash:<family>:v2:<…>` — never a mutation
  in place. Old readers keep working against old keys until every consumer
  has moved; then the old keys are retired in the registry.
- The contract itself is versioned by this repo's git history; the registry
  records per-family status (`live`, `migrating`, `retired`).

## Consumer conduct

Read-only (CD-5); degrade-don't-block (CD-6); trust published state only
while fresh; validate at the choke point and skip bad records. The shared C
library (sprint 002) implements this once — dashboards should consume feeds
through it rather than hand-rolling hiredis loops.
