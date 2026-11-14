# Sprint 024 — `claude:limits`: represent "no five-hour window"

**Proposal:** korg:4633 (slice 6 of program korg:4619, kpidash row 2)
**Covers:** WI 4630
**Branch:** `024-claude-limits-no-five-hour`
**Run as:** karc leg `kdashdata-18f7ec` on kai, overseen from cleo

## Goal

On 2026-11-14 the oauth usage endpoint returned `"five_hour": null` for Ken's
account. `seven_day` and the scoped Fable window were present, and `limits[]`
had no five-hour entry (raw read on kdeskdash WI 4623, comment 4662). Ken
confirmed from the desktop app that his plan no longer has a five-hour window,
and ruled (comment 4665): with no window, draw seven-day and Fable; with one,
draw as today. The `claude:limits` contract could not say "no window".
`five_hour_pct` is required, so a writer could only publish 0 or a wrong
number. The live publisher published a wrong one, copying seven-day values
into the five-hour fields.

This slice is the contract and the C reader. The publisher fix (korg:4634)
and kpidash's second card layout (korg:4635) are built on it next.

## Premise check

Run on kai, 2026-11-13 17:20 PST.

- **4630 holds.** The schema had no field for an absent window, and
  `five_hour_pct` was required. `kdash_parse_claude_limits()` rejected a hash
  without it. `claude-pub.sh` in kdeskdash has three headline writers
  (statusline, history file, oauth), as WI 4623 describes.
- No guiding plan lists kdashdata.
- **The notes' registry repair has drifted.** The notes said korg:4616 had
  shipped and asked for the `kdash:korg:pulse` "Writer, not yet" text to name
  `bin/korg-pulse`. It has not shipped: korg PR #110 is open, and
  `bin/korg-pulse` is not on korg's `origin/main`. Those rows are left as they
  were, and the drift is flagged on the proposal (comment 4668).

## Decisions — CD-29

- **A flag, `five_hour_none` ∈ {0, 1}, not a dropped field.** Removing the
  required `five_hour_pct` is breaking (rules.md). Every pinned reader would
  reject the whole record, including the seven-day gauge. A no-window write
  therefore still carries `five_hour_pct 0`. Old readers draw 0 %, and new
  readers know better.
- **The flag names the missing window, not the present one.** A hash from a
  pre-change writer has no flag, and must read as "has a window". A zeroed
  (rejected or unread) struct must not read as "no window". With
  `five_hour_none`, both fall out without special cases.
- **Every headline writer states the flag on every headline write**, in the
  same HSET as `updated_at`. The writer that observes no window clears the
  five-hour leftovers: it writes `five_hour_pct 0` and `five_hour_resets_at 0`.
  This is the one exception to "a source that does not know the reset leaves
  it alone". A writer that observes a window writes `five_hour_none 0`, which
  clears the flag. All three writer paths can tell which case they are in,
  so none needs a "leave it alone" case.
- **The reader does not trust the cleanup.** It honours only exactly `1`,
  and when the flag is set it zeroes both five-hour fields whatever the hash
  holds. The flag rides `updated_at`, so the existing staleness call covers
  it.
- Rejected: a `claude:limits:v2` key, and a `-1` sentinel in `five_hour_pct`.
  The reasons are in CD-29.

## What shipped

- `contracts/schemas/claude-limits.schema.json`: `five_hour_none`, the
  `five_hour_resets_at` exception, a no-window example (the live 01:13Z
  record's shape), and two counterexamples. One is a boolean flag. The other
  is a no-window record that drops `five_hour_pct`, which the old readers
  would reject whole.
- `include/kdash/kdash_payload.h` / `src/kdash_payload.c`:
  `kdash_claude_limits_t.five_hour_none`. The parser sets it on exactly 1 and
  zeroes the five-hour fields beside it.
- `tests/test_payload.c`: these cases are covered:
  - a pre-change hash;
  - a no-window record;
  - a stale leftover `37` / reset beside the flag, which is zeroed;
  - flag 0, and junk flags (`true`, `2`, `1.0`, empty), which leave the
    window alone;
  - a flag on a record that lacks `five_hour_pct`, which still rejects;
  - an old no-window statement, which is stale, and a fresh one, which is
    not.
- `examples/kdash_dump.c` prints `five-hour none`.
- `contracts/registry.md`: the writer/reader table for the three cases, the
  clearing rule, and the `claude:limits` row.
- `docs/architecture.md`: CD-29.

**Negative tests.** With the parser's zeroing removed, `test_payload` fails
at "are zeroed, whatever the hash holds". With the example's flag planted as
`2`, `check-docs` fails with `five_hour_none: 2 is not one of [0, 1]`. Both
were restored and the gates are green.

## What a publisher writes, and what a reader sees

| the source shows | publisher writes (same HSET as `updated_at`) | C reader sees |
|---|---|---|
| a five-hour window | `five_hour_none 0`, real `five_hour_pct`, `five_hour_resets_at` if known | `valid`, `!five_hour_none`, values as published |
| no five-hour window (oauth `five_hour: null`; statusline `rate_limits` without `five_hour`; history sample without `fh`) | `five_hour_none 1`, `five_hour_pct 0`, `five_hour_resets_at 0` | `valid`, `five_hour_none`, both five-hour fields 0 whatever the hash holds |
| pre-change writer | no flag | `valid`, `!five_hour_none` |
| nothing usable | — | `KDASH_ABSENT` / `KDASH_UNAVAIL` or `!valid`, zeroed, so `five_hour_none` is false |

The one gap: a pre-change writer that writes after a no-window write leaves
the flag set beside its own five-hour value. The reader hides that value, and
the only such writer is today's buggy `claude-pub.sh`. The gap closes when
korg:4634 deploys.

## Repaired in passing

- `contracts/registry.md` § Reserved listed the `kdash:` families "so far" as
  ending at `update` and `nas`, with "`korg` is decided but not yet written".
  Sprints 021 and 023 added `weather`, `korg` and `klams`, and their rows are
  in the registry's own table. The list now names all three. `check-docs` is
  green.

## Follow-ups

- The `kdash:korg:pulse` writer text, once korg PR #110 merges. Replace
  "a korg-reading publisher, cadence in `expected_refresh_s` (not yet — see
  below)" with "korg's `bin/korg-pulse` on kai, every 5 min", and the section's
  "Writer, not yet" sentence to match. This is sequencing, not a decision.
  The overseer holds it (comment 4668).

## Deployed

2026-11-14 01:25Z (2026-11-13 17:25 PST), from merged `main` `d8c40a4` on kai,
by `.sprint-deploy`'s `recipe: publish` then `recipe: deploy`.

- `just publish`: "nothing to publish: 0.1.0-4837473 already in the store".
  This sprint changed no publisher input (`publishers/rust/` untouched), so
  the self-skip is the correct outcome.
- `just deploy`: kdash-pub 0.1.0-4837473, unchanged, confirmed on kai,
  kubs0, cleo, kimac and thrall. komarchy and kbook2 timed out and
  thrall-arch did not resolve; all three are `--host-optional` and were
  reported SKIPPED, rc 0. The probes ran from kai, through knarr.
- What this sprint shipped is the contract and libkdash. Nothing in the fleet
  runs them until consumers re-pin: kdeskdash's `claude-pub` (korg:4634) and
  kpidash's card (korg:4635) pin `d8c40a4`. No live check of `five_hour_none`
  is possible until korg:4634's writer deploys.
- korg PR #110 was still open at ship time, so the `kdash:korg:pulse` writer
  text was left as it was (see Follow-ups).
