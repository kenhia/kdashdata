# 008 — The `kdash:stale:<host>:<deployer>` feed

korg: proposal 1915, work item 1904. **Slice 2 of the karc acceptance
program** (korg:1919), run as an overseen sprint in karc leg
`kdashdata-77f56b` on kai. Slice 1 (k-homelab, korg:1914) declares which hosts
are intermittent; slices 3 and 4 write this feed; kpidash reads it.

## Goal

komarchy is a laptop. It is off more often than it is on, and every deployer
that tries to push to it either fails the whole run or skips it silently —
both wrong. Slice 1 taught k-homelab to declare a host intermittent and skip
it deliberately. This slice gives the skip somewhere to be *recorded*, so an
operator can see what is behind on that machine and a dashboard can say so.

Contract only: a registry entry, a JSON schema, and the decision behind them.
No C reader, no publisher change — the feed is registered ahead of its
consumers on purpose, because slices 3 and 4 need something to write against.

## Premise check at the brief

Every falsifiable claim in #1904 held, except one that turned out to name
something this repo does not have.

- **`kdash:*` is the namespace for new shared feeds, schema-per-feed, ts
  everywhere.** All three confirmed in `contracts/rules.md`; `contracts/schemas/`
  holds ten schemas and the docs gate enforces that each is registered.
- **The feed is not already registered.** `grep -i stale` over `contracts/`
  found only prose about *reader-owned staleness* in other families — no
  `kdash:stale:` key anywhere.
- **"Doc-only, no consumer-lib dependency" is achievable.** True, and it stays
  true: nothing in `include/kdash/` had to move.
- **The publisher half needs no new code — and it needs less than #1904
  assumed.** The item says a deployer "SETs on skip and DELs on a verified
  sync". Both verbs already ship: `kdash` is in the namespace allowlist of both
  wrappers (`publishers/rust/src/keys.rs:28`, `publishers/python/.../keys.py:23`)
  and `del` is an existing verb (`publishers/rust/src/command.rs:140`). The
  whole write path for slices 3 and 4 is two one-line commands.
- **Premise drifted: there is no "existing schema test harness".** The proposal
  asks to "validate the schema against the repo's existing schema test
  harness". There isn't one. `scripts/check.py` proves a schema *parses* and is
  *registered*; `tests/test_payload.c` tests the C parsers, which only exist for
  feeds that have a C reader — and this one deliberately does not. Nothing in
  the repo validates a payload against a schema, and no `jsonschema` dependency
  exists to do it with. Reported rather than fixed: inventing a validator would
  add a dependency to make a gate, which this repo's conventions forbid, and it
  is a bigger decision than a contract slice should take alone. See Follow-ups.

kdashdata is not in the cross-project-planning routing table, so no guiding
plan applies.

## Decisions

### The freshness rule is neither of the two rules.md offers (now CD-18)

This is the sprint's one real decision, and it is the one a reader is most
likely to get wrong, because the feed's *shape* is identical to `services`:
a JSON string, no TTL, a `ts` field. Everything about it says "ts-owned", and
ts-owned is exactly what it must not be.

`services` ages out because a silent writer there means "nobody knows". Here a
silent writer means the host is **still** unreached. Apply a staleness window
and the longest outages produce the greenest dashboards — a precise inversion
of the feed's purpose. Apply a TTL and it is worse, because a TTL is a
self-clearing flag and the only thing permitted to clear this one is a verified
sync. The TTL guidance in rules.md is "≈ 3× write cadence", and there is no
cadence to multiply: a skip is an event, not a heartbeat.

So presence is the truth. `SET` on a skip, `DEL` on a sync, absence is the only
all-clear, and a deployer that is never run again correctly leaves its flag
standing forever. CD-18 states the rule generally, because the next feed whose
*presence* means "something is wrong" inherits it.

### A malformed payload must not clear the flag

The part worth writing down, because it contradicts a rule that is otherwise
absolute here. Every reader in this repo validates at the choke point and skips
a record it cannot parse (rules.md; CD-6). Skipping *this* record renders as
all-clear — one parse bug and a three-week outage reports healthy.

So the key carries the signal and the payload only enriches it. A reader that
cannot parse the payload still reports the host stale, and drops the detail.
That inversion is in CD-18, in the registry entry, and in the schema's own
`description`, because the reader most likely to get it wrong is one that has
already written seven readers the other way.

### `stale` is pinned `const: true`, not merely documented as always-true

#1904 says outright that "the key's presence is the flag", which makes the
field redundant. The proposal's carry-forward asked to keep it for readability
while stating that nobody may treat `stale: false` as meaningful.

`const: true` says that structurally instead of in prose. The field survives so
a human running `GET` reads a sentence rather than three bare numbers, and
`stale: false` becomes off-contract rather than merely discouraged — which
matters because the writer tempted to publish it is a writer trying to *clear*
the flag, and publishing anything at all leaves the key, and therefore the
flag, up. There is no false. Clearing is a `DEL`.

### Two stamps in the payload, and a third clock deliberately outside it

`since` is the first skip of the current run; `ts` is the last write. Neither is
a last-contact time, and the registry says so explicitly, because k-homelab
keeps that third clock itself as a git-ignored per-checkout `.state/last-seen`
(slice 1) — deliberately not in Redis, so `bin/audit` never blocks on it.

After a three-week outage `since` and `last-seen` differ by the whole outage.
Both end up in operator-facing text, so the schema pins the distinction on each
field rather than leaving it to whoever writes the card. `since` also carries a
writer obligation the schema cannot enforce: carry it unchanged across later
skips. A writer that restamps it on every run turns "stale for three weeks"
into "stale for an hour" — the failure would look like correct code.

### All four fields required, and both stamps positive

Cheap to require, because a validation failure here cannot clear the flag —
the key already did the signalling. So the required set is a clear instruction
to slices 3 and 4 rather than a trap. Both stamps get `exclusiveMinimum: 0`
following `kdash-panel`'s precedent: both are rendered as durations, and a `0`
renders as a plausible-looking "stale since 1970" rather than as the missing
stamp it is. `reason` gets `minLength: 1` for the same reason — an empty string
renders as a blank line, which is worse than a missing field.

### No C reader, and what the day it lands needs

The other two `kdash:*` feeds are read with a single `GET` on a key the reader
builds itself. This one is **discovered**: "is anything stale on komarchy" is a
`SCAN` over `kdash:stale:<host>:*`, and both identity segments come off the
key. So the day a dashboard consumes it through libkdash it needs a parse as
well as a reader — the choke point sprint 007 deliberately omitted for `panel`,
for the opposite reason. Noted in the registry so it is not mistaken for an
oversight.

## What shipped

**Contract**
- `contracts/schemas/kdash-stale.schema.json` — the source of truth. `stale`
  (`const: true`), `since` and `ts` (numbers, `exclusiveMinimum: 0`), `reason`
  (`minLength: 1`), all four required, `additionalProperties: true`. The
  `description` carries the two inversions, because the schema file is what a
  sibling repo reads.
- `contracts/registry.md` — the family row and the entry: the pairing rationale,
  where `{host}` comes from, the presence-is-the-flag rule, the
  malformed-payload inversion, the two stamps against the third clock, the
  `const` reasoning, both writer one-liners, and what a future C reader needs.
  The Reserved section now counts three families.
- `docs/architecture.md` — **CD-18**, including what the decision costs.

**Not shipped, deliberately**: no C reader, no publisher change, no new gate.

## Negative tests

The docs gate first, on the surfaces this sprint touched. One of the three was
not planted — writing the schema before the registry entry produced it for
free, which is the order the gate exists to enforce.

| Planted fault | Caught by |
|---|---|
| Schema lands with no registry entry (natural, not planted) | `contracts/registry.md: no link to schemas/kdash-stale.schema.json` |
| Registry links `kdash-staleness.schema.json` (a typo'd filename) | broken link **and** unregistered schema — 2 problems |
| A doubled comma in the new schema | `invalid JSON: Expecting property name … line 6` |

Then the schema itself, against the ambient `jsonschema` 4.10.3 on kai — a
sprint-time check, **not** a gate and not a new dependency (`.scratch/`, not
committed). Thirteen cases, all as documented:

| Case | Result |
|---|---|
| conforming record; unknown extra field; float stamps | accepted (the additive-evolution rule holds) |
| `stale: false` — the clear-by-payload trap | rejected |
| `stale` / `since` / `ts` / `reason` missing | rejected |
| `since: 0`, `ts: 0` | rejected |
| `since` as a string; payload not an object | rejected |
| `reason: ""` | rejected |

The schema is also a legal draft 2020-12 schema (`check_schema`), which is more
than `just check` can tell you — the docs gate only proves it is legal *JSON*.

## Verification of the write path

The registry makes three claims about how a deployer writes this feed. All
three checked against the shipped wrappers rather than assumed:

- **The key passes.** `kdash:stale:komarchy:k-homelab` and
  `kdash:stale:komarchy:agent-skills` both clear `check_key`; a space in the
  deployer segment and an empty segment are both rejected.
- **`ts` is stamped and `since` is not touched.** `payload.stamp` adds `ts`,
  leaves `since` exactly as written, and does not overwrite a `ts` the writer
  supplied — the rules.md publisher obligation, confirmed rather than trusted.
- **`del` exists**, so the clear half needs no new code either.

One honest limit found while checking: the wrappers validate the namespace and
the token charset, **not** this family's segment count.
`kdash:stale:komarchy` — deployer left off — publishes without complaint, and
no reader will ever look at it. That is by design (rules.md puts arity at the
reader's choke point) and it is the same shape as sprint 007's finding that the
publisher does not police the payload schema. It is now stated in the registry,
because it is a live foot-gun for slices 3 and 4.

Nothing was published to the central Redis: this sprint adds no reader to
verify against and no writer to verify with, so a live round-trip would have
been theatre. Slices 3 and 4 do it for real.

`just check` green — all four gates, both publisher wrappers, the C library and
its four ctest suites.

## Follow-ups

- **No schema-validation harness exists** (premise drift above). Every schema
  in this repo is prose-checked and parse-checked, and nothing has ever
  validated a payload against one. Worth a work item, and worth deciding
  deliberately: the honest fix needs `jsonschema`, which this repo has so far
  avoided, and the convention here is to add no dependency to make a gate. An
  alternative is a per-schema `examples/` block of valid and invalid payloads
  plus a stdlib checker narrow enough to cover the constructs actually used
  (`const`, `required`, `exclusiveMinimum`, `minLength`, `enum`). Not this
  sprint's call to make alone.
- **Slice 3** (agent-skills, korg:1916) and **slice 4** (k-homelab, korg:1917)
  write the feed; **kpidash #1903** reads it; **kmon #1921** may later read it
  instead of filing a finding. All four should be held to the two inversions —
  no window, and a bad payload never clears the flag.
- **A C reader needs a SCAN and a parse** when a dashboard wants this through
  libkdash. See Decisions; not needed while kpidash reads it its own way.
