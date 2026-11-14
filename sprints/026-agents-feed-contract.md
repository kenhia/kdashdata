# Sprint 026 — The agent-session timeline feed (`kdash:agents`)

**Proposal:** korg:4696 (slice 1 of program korg:4699, kpidash Agents card)
**Covers:** WI 4693
**Branch:** `026-agents-feed-contract`
**Run as:** karc leg `kdashdata-1dcc9a` on kai, overseen from cleo

## Goal

kpidash's row 3 gets a 4×1 Agents card: a 24-hour timeline of agent
sessions, with host-coloured bars, legs hung under their overseer, ship marks,
and yellow for a session waiting on Ken. This sprint defines the feed it
reads and ships libkdash's parser and reader for it, the way sprint 025 did
the calendar. Two slices build on it in parallel: ksitrep's activity spans
and publisher (korg:4697) and kpidash's card (korg:4698).

## Premise check

Run on kai, 2026-11-14 11:35 PST.

- **4693 holds.** No `kdash:agents` key, schema, registry row or reader
  existed. `kdash` is in both publisher allowlists, so `kdash-pub set
  kdash:agents …` needs no publisher change, and `check-docs`' family
  comparison is unaffected.
- **The `VALUE_MAX` question in 4693 is real.** A full-cap record does not
  fit 16 KB (measured below).
- No guiding plan lists kdashdata (`cross-project-planning/index.md`).

## Decisions

Recorded as **CD-31** in `docs/architecture.md`. WI 4693 proposed the shape;
it landed as proposed, with these additions:

- **Key `kdash:agents`**, v1, ts-owned, no TTL, central, no `v` field
  (CD-28). `expected_refresh_s` (60) and `window_s` (3600..604800; 86400
  for the card) are required. A record without a window is rejected.
- **A running bar reaches now only while the record is fresh.** The proposal
  notes said a consumer clips `end` to now and runs `working`/`waiting` bars
  to now. This sprint adds the stale half: once `ts` is past its cadence, a
  running bar stops at its `end`, so a dead publisher cannot keep growing
  bars. `kdash_agents_bar_end()` carries the whole rule.
- **`id` and `host` use the host-token charset** (`[A-Za-z0-9._-]`, ≤ 23 and
  ≤ 15). **`name` is any UTF-8, ≤ 31 characters**, and the C reader counts
  characters, as the schema does, in a 128-byte buffer.
- **`parent` must resolve to another kept bar whose `overseer` is true**, or
  the link is dropped and counted (`parents_dropped`), and the bar is kept.
  Links resolve after truncation, and `parent_index` gives the slot.
- **Past 96 bars the newest are kept**, because the card's right edge is now.
  Duplicate ids keep the first.
- **A bar is skipped and counted** for an unknown `state`, `start` > `end`,
  an `end` before the window, a malformed field, or a repeated `id`. A bar
  whose `start` predates the window is kept unclipped.
- **`VALUE_MAX` rose from 16 KB to 64 KB.** A full-cap record with ASCII
  names is 22,638 bytes. The schema counts characters, so a writer whose
  encoder escapes non-ASCII (Python's `json.dumps` default) can spend 12
  bytes on one astral character, about 55 KB in all. `test_feed` reads that
  record whole.

## What shipped

- `contracts/schemas/kdash-agents.schema.json`: three examples (an overseer
  with a leg that shipped and a waiting session, an empty record, and a
  non-ASCII name with `v` and an unknown field) and fourteen
  counterexamples.
- `contracts/registry.md`: a table row and a section.
- `docs/architecture.md`: CD-31.
- libkdash:
  - `KDASH_KEY_AGENTS` (`kdash_keys.h`)
  - `kdash_agents_t`, `kdash_agent_bar_t`, `kdash_agent_state_t`,
    `kdash_parse_agents()`, `kdash_agent_state_from_str()`,
    `kdash_agent_state_str()` and `kdash_agents_bar_end()`
    (`kdash_payload.h`, core)
  - `kdash_agents()` (`kdash_feed.h`)
  - an agents section in `kdash_dump`
- Tests:
  - `test_payload`: an overseer with a leg and a waiting bar; right edges,
    fresh and stale; the state words; an empty record; fourteen kinds of bad
    bar skipped beside one good one; character-counted names; a bar
    straddling the window; five kinds of bad parent link dropped and a
    forward link resolved; 100 bars truncated to the newest 96, which drops
    the evicted overseer's link; exactly 96 bars; and seven record-level
    rejections
  - `test_feed`: OK (and that the GET names `kdash:agents`), UNAVAIL,
    ABSENT, a windowless record read as ABSENT, and the worst-case full-cap
    record measured over 32 KB and under `VALUE_MAX` and read whole

## Evidence

- `just check` green on kai, 2026-11-14 (check-docs: 24 schemas; Python 60
  tests; Rust 63+15+1; ctest 5 of 5), and `just build-aarch64` clean.
- **Negative tests (C).** Twelve defects were planted one at a time, and each
  failed `ctest` before the code was restored:
  - `VALUE_MAX` back at 32 KB
  - the reader GETting the wrong key
  - a parent that need not be an overseer
  - a bar parenting itself
  - the window check dropped
  - an inverted bar kept
  - duplicate ids kept
  - truncation keeping the oldest
  - names counted in bytes
  - staleness ignored by `kdash_agents_bar_end()`
  - an unknown state accepted
  - a malformed parent left uncounted
- **Negative tests (schema).** Each of these made `check-docs` fail on its
  counterexample: raising `name`'s `maxLength` to 32, dropping `shipped`
  from `required`, and adding `idle` to `state`'s enum.
- **Live read.** `kdash_dump` ran on kai with `/etc/khomelab/secrets.env`
  sourced. It reported `agents: no record`, which is the correct ABSENT
  result because ksitrep does not write the key yet. **Nothing was written to
  Redis.**

## Repaired in passing

- **The calendar reader rejected schema-valid non-ASCII names.**
  `kdash_calendar_day_t`'s `holiday`, `special` and `short` buffers were
  sized in bytes (32/32/13), while the schema bounds them in characters. A
  31-character holiday with one accent, or a 12-character `short` like
  `Día de Ánimo`, was skipped. The buffers now hold their bound in four-byte
  characters, and `opt_tag` counts characters. A new `test_payload` case
  covers it, and putting `KDASH_CALENDAR_NAME_MAX` back to 32 failed it.
  `kdash_calendar_t` grows from about 6 KB to about 20 KB. Gate: `ctest`.
- **The registry still said `kdash:calendar` had no writer.** korg sprint 101
  (`c49e2b6`) shipped it on 2026-11-14, and the live record's `ts` was 29
  minutes old with nothing changed, so it rewrites at least hourly. The table
  row and the section now name korg's `korg-calendar` timer. Gate:
  `check-docs`.
- **Both orientation files' `Next:` line led with korg:4667 and korg:4668**,
  and both are `done`. It now leads with this program's korg:4697 and
  korg:4698. Gate: `check-docs`.

## Follow-ups

- korg:4697 and korg:4698 pin this sprint's merge commit, named in the wrap-up
  handoff on korg:4696.
