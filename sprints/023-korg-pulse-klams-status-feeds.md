# Sprint 023 — Contracts for the korg pulse and klams status feeds

**Proposal:** korg:4615 (slice 2 of program korg:4619, kpidash row 2)
**Covers:** WI 4610
**Branch:** `023-korg-pulse-klams-status-feeds`
**Run as:** karc leg `kdashdata-29dd62` on kai, overseen from cleo

## Goal

Contract the two feeds behind kpidash's combined korg + klams card (mock-up
`kai:/scratch/kpidash-row2-mockup/row2-cards-mockups.html`, cards "KORG +
KLAMS", "KORG PULSE" and "KLAMS"). The fuller field set the standalone cards
show is contracted now, so a standalone card later needs no version bump.
Three slices build on this one: the korg publisher (korg:4616), the klams
publisher (korg:4617) and the kpidash widget (korg:4618).

Unlike sprint 021's weather contract, which shipped schema and registry only,
this brief asked for the libkdash side too: a key constant, a parser and a
feed read for each feed, with tests.

## Premise check

Run on kai, 2026-11-13.

- **4610 holds.** No `korg` or `klams` schema or registry row existed. The
  only `kdash:korg:*` content was CD-26's two provisional, unbuilt keys.
  `kdash` is in both publisher allowlists, and `check_key` accepts both new
  keys unchanged.
- No guiding plan lists kdashdata (`cross-project-planning/index.md`).
- The mock-up's own data matches the brief's field list, with one addition.
  The card's per-project bar has one segment per entry, so a cap needs a
  named overflow. That became `other_open`.

## Decisions

All of these are recorded as **CD-28** in `docs/architecture.md`. The brief
left key names, exact field names and bounds to this leg where the repo's
conventions say otherwise. Where a convention said otherwise, it is named
below.

- **Keys:** `kdash:korg:pulse` and `kdash:klams:status`, the brief's
  suggestions, both v1. The pulse is a third key in CD-26's family rather
  than an early build of `board`/`rate_of_fire`, because the card needs one
  snapshot spanning both of those reads.
- **No `v` field.** CD-27 puts the version in the key
  (`kdash:korg:v2:pulse`). A writer may still send `v`, and readers ignore
  it.
- **`expected_refresh_s` is required**, and the library gained
  `kdash_cadence_stale()` plus `KDASH_CADENCE_GRACE_S` (60 s). This is the
  claude:limits rule, made reusable.
- **`open` is korg's `backlog`** (non-terminal, unarchived), and `history`
  is the same figure from `work_item_flow`. Copied, never recounted (CD-26).
- **`by_project` is capped at 24, largest first**, and `other_open` carries
  the overflow plus any unassigned items. It is required even when it is 0.
- **`last_ran_at` and `new_per_day` are required**: the first nullable, the
  second possibly empty. The brief said optional for both. CD-27's
  present-and-null rule decided it.
- **klams `down` is a record, not a silence.** The publisher writes
  `health: "down"` with nulls, so a stale `ts` and a dead klams stay
  distinguishable.
- **In C, a cap truncates and flags; it does not reject.** The first N of a
  largest-first list are kept, and the newest N of an oldest-first series.
  Null counts read as `KDASH_COUNT_NULL` (-1), null stamps as 0.

## What shipped

- `contracts/schemas/kdash-korg-pulse.schema.json`: three examples (the
  mock-up's card, an empty korg, every cap full) and six counterexamples.
  The counterexamples are an absent `other_open`, a 25th project, a
  millisecond `ts`, a locale date, a zero-open project and a missing cadence.
- `contracts/schemas/kdash-klams-status.schema.json`: three examples (`ok`,
  `degraded` with null run times, `down`) and five counterexamples. The
  counterexamples are the mock-up's `warn` used as a wire word, an absent
  `last_ran_at`, a `down` record that drops a count, a millisecond
  `backup_at` and 15 days of `new_per_day`.
- `contracts/registry.md`: two table rows, a section for each feed, and a
  note in the provisional korg section pointing at its first built key.
- `docs/architecture.md`: CD-28.
- libkdash:
  - `KDASH_KEY_KORG_PULSE` / `KDASH_KEY_KLAMS_STATUS` (`kdash_keys.h`)
  - `kdash_parse_korg_pulse()`, `kdash_parse_klams_status()` and the klams
    health enum (`kdash_payload.h`)
  - `kdash_korg_pulse()` and `kdash_klams_status()` (`kdash_feed.h`)
  - `kdash_cadence_stale()` (`kdash_freshness.h`)
  - a status section in `kdash_dump`
- Tests:
  - `test_payload`: a full record, a minimal record, an empty `history`
    beside a real backlog, over-long `by_project` and `history` (the first
    24 and the newest 30 kept), malformed entries skipped and counted,
    required-field rejections, and for klams the `down` record, null vs
    absent `last_ran_at`, and over-long `new_per_day`
  - `test_freshness`: the cadence boundaries
  - `test_feed`: both readers' OK, UNAVAIL and ABSENT results, a
    schema-invalid record read as ABSENT, and a record at every cap shown to
    fit `VALUE_MAX`
- No publisher change. `kdash` was already allowlisted, so the write is
  `kdash-pub set kdash:korg:pulse '<json>'`.

## Evidence

- `just check` green on kai, 2026-11-13. The docs gate reports 22 schemas
  registered and validating. 60 Python tests, 79 Rust tests (63 + 15 + 1)
  and 5 of 5 ctest suites passed. `just build-aarch64` built the library
  and `kdash_dump` with no warnings.
- **Negative tests.** Five defects were planted one at a time, and each
  failed its test before the code was restored:
  - history keeping the oldest 30 instead of the newest
  - a null count read as 0
  - an absent `last_ran_at` accepted as null
  - `VALUE_MAX` cut to 2048
  - `kdash_cadence_stale` ignoring the writer's cadence
- **Schema gate.** Raising `by_project`'s `maxItems` to 25 made
  `check-docs` fail on the counterexample it exists for.
- **Live read.** `kdash_dump` from kai, with the CD-12 env file sourced,
  connected to central and reported `korg: no record` and `klams: no
  record`. That is ABSENT, correctly distinct from unavailable, because
  nothing writes either key yet. **Nothing was written to Redis.**

## Repaired in passing

- `check-docs` requires both orientation files' Status paragraphs to name
  the newest sprint record. Both now describe sprint 023, cite CD-1…CD-28,
  and list the three downstream slices under Next.
- `kdash_feed.c`'s `VALUE_MAX` comment still said the biggest payload was
  "of the five" kpidash feeds. It now names the largest record a schema
  allows, about 3 KB, and the test that measures it.

## Follow-ups

- korg:4616, korg:4617 and korg:4618 pin this sprint's merge commit (named
  in the wrap-up handoff on korg:4615).
- WI 4612 (why kubs0's scanner stopped writing) decides which scanner time
  the combined card draws. Both are on the wire, so the answer needs no
  contract change.
