# Sprint 025 — The calendar feed contract (`kdash:calendar`)

**Proposal:** korg:4666 (slice 1 of program korg:4669, kpidash row 3)
**Covers:** WI 4663
**Branch:** `025-calendar-feed-contract`
**Run as:** karc leg `kdashdata-fb6407` on kai, overseen from cleo

## Goal

kpidash is getting a 2×1 time and calendar card at the right end of row 3
(mock-up A, 24-hour clock). The panel draws the clock and the month grid
itself. This feed carries only the **tagged days**: Ken's days off, holidays
and special days. Contract, schema and consumer code, the way sprint 023 did
the korg pulse. Two slices build on it: korg's tagged-days list and publisher
(korg:4667) and kpidash's card (korg:4668).

## Premise check

Run on kai, 2026-11-14 00:40 PST.

- **4663 holds.** No `calendar` key, schema, registry row or reader existed.
  The only matches for "calendar" were the weather feed's prose. `kdash` is in
  both publisher allowlists, so `kdash-pub set kdash:calendar …` needs no
  publisher change.
- No guiding plan lists kdashdata (`cross-project-planning/index.md`).
- kpidash consumes this repo as the `lib/kdashdata` submodule and links
  `kdash_core` only (its own hiredis layer does the GET). The parser and the
  two lookups are therefore in `kdash_payload.c`, which is core.

## Decisions

Recorded as **CD-30** in `docs/architecture.md`. WI 4663 proposed the shape
and left the spelling to this leg.

- **Key `kdash:calendar`**, v1, ts-owned, no TTL, central. It is
  provider-neutral, like `kdash:weather:{loc}`, because the content is days,
  not korg figures (CD-26's family copies korg's own numbers). A breaking
  change is `kdash:calendar:v2`.
- **No `v` field**, following CD-28. This is the one departure from WI 4663's
  sketch. A writer may still send it, and readers ignore it.
- **`expected_refresh_s` is required** (3600 for an hourly publisher), so
  `kdash_cadence_stale()` can give the muted hint. It never blanks the card.
- **`from`/`to` are required and inclusive.** A window that no longer covers
  today is the real stale condition, tested by `kdash_calendar_covers()`.
- **Dates are America/Los_Angeles**, named in the contract, because the
  fleet's Linux hosts run UTC.
- **`off` is a required boolean.** `holiday`, `special` and `short` are
  optional strings, absent when there is no such tag, never null or empty.
  CD-27's present-and-null rule covers unmeasurable values, and an untagged
  date is fully known.
- **Bounds:** `holiday`/`special` ≤ 31 characters, `short` ≤ 12, `days` ≤ 64.
- **Observed holidays are their own entries** named `… (observed)`, rather
  than a flag.
- **In C** a record with a missing, malformed or inverted window is rejected.
  An entry that is malformed, untagged or outside the window is skipped and
  counted. Past 64 kept entries the earliest are kept and `days_truncated`
  is set.
- **`VALUE_MAX` rose from 8192 to 16384.** A full-cap calendar is 9,378
  bytes, over the old clamp. The clamp sits on a reply hiredis has already
  allocated, so raising it costs nothing.

## What shipped

- `contracts/schemas/kdash-calendar.schema.json`: three examples (this
  month as the program's acceptance names it, an empty list, an observed
  holiday carrying all three tags plus an unknown field and `v`) and eight
  counterexamples (`off` absent, `holiday: null`, a 16-character `short`, a
  locale date, `off` as a word, `to` absent, a millisecond `ts`, the cadence
  absent).
- `contracts/registry.md`: a table row and a section.
- `docs/architecture.md`: CD-30.
- libkdash:
  - `KDASH_KEY_CALENDAR` (`kdash_keys.h`)
  - `kdash_calendar_t`, `kdash_calendar_day_t`, `kdash_parse_calendar()`,
    `kdash_calendar_covers()` and `kdash_calendar_day()` (`kdash_payload.h`,
    core)
  - `kdash_calendar()` (`kdash_feed.h`)
  - a calendar section in `kdash_dump`, which takes today in
    America/Los_Angeles
- Tests:
  - `test_payload`: this month's four tagged days, with the 27th's day off
    and birthday in one entry; an entry with all three tags; an empty
    `days`; inclusive window ends and the outside-window stale case; eleven
    kinds of bad entry skipped and counted beside one good one; 70 entries
    truncated to the earliest 64; seven record-level rejections; and NULL
    and zeroed-record lookups
  - `test_feed`: OK (and that the GET names `kdash:calendar`), UNAVAIL,
    ABSENT, a windowless record read as ABSENT, and a full-cap record
    measured over 8 KB and under `VALUE_MAX` and read whole

## Evidence

- `just check` green on kai, 2026-11-14, and `just build-aarch64` clean.
  The figures are in the wrap-up handoff on korg:4666.
- **Negative tests (C).** Seven defects were planted one at a time, and each
  failed its test before the code was restored:
  - an untagged entry kept
  - the window check dropped
  - an inverted window accepted
  - truncation keeping the newest entry
  - `holiday: null` read as absent
  - `VALUE_MAX` back at 8192
  - the reader GETting the wrong key
- **Negative tests (schema).** Raising `short`'s `maxLength` to 16 made
  `check-docs` fail on its counterexample. So did dropping `off` from
  `required`.
- **Live read.** `kdash_dump` ran from kai with `/etc/khomelab/secrets.env`
  sourced. It connected to central, read the korg pulse and klams status
  live, and reported `calendar: no record`. That is ABSENT, which is correct
  because nothing writes the key yet. **Nothing was written to Redis.**

## Repaired in passing

- Both orientation files' `Next:` line still led with five proposals that had
  all shipped: korg:4634, 4635, 4616, 4617 and 4618, all `done` in korg on
  2026-11-14. Those were removed, and the line now leads with this program's
  korg:4667 and korg:4668. Gate: `check-docs`.
- `test_feed`'s korg-pulse size check compared against a literal 8192 while
  its message said `VALUE_MAX`. Both size checks now use one
  `FEED_VALUE_MAX`, which mirrors the private constant.

## Follow-ups

- korg:4667 and korg:4668 pin this sprint's merge commit, named in the wrap-up
  handoff on korg:4666.
