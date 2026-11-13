# Sprint 021 — The weather feed contract: `kdash:weather:{loc}` v1

**Proposal:** korg:4575
**Covers:** WI 4573
**Branch:** `021-weather-feed-contract`
**Run as:** an interactive session on cleo, working on kai through kaed

## Goal

Put the weather feed on contract: one schema file and one registry entry.
The collector (kweather, korg project 85, born the same day) and the schema
were written first, in the session that designed the feed with Ken; this
sprint moves the schema in-tree so the collector can publish on-contract and
kpidash can build its filler card against a schema it vendors.

## Premise check

Run on kai, 2026-11-12.

- **4573 holds.** No `weather` schema or registry row existed (`ls
  contracts/schemas`, grep of `registry.md`). The source schema is at
  `kai:~/src/homelab/kweather/kdash-weather.schema.json` and was copied
  byte-for-byte (md5 `490528d4…` on both sides).
- No guiding plan lists kdashdata (`cross-project-planning/index.md`), so
  nothing there constrains this.

## Decisions

- **No new CD entry.** The WI asked for the call. Everything structural here
  is CD-27 applied again: versioned by key, carries what the one consumer
  draws, present-and-null. What is new is specific to this feed — a
  provider-neutral payload with a collector-classified `cond`, and a `{loc}`
  token where other families have `{host}` — and it is recorded in the
  registry section, where a reader of the feed will look. A decision that only
  binds one feed does not need a number.
- **`{loc}` reuses the host-token grammar** rather than introducing a new
  token class. The publishers' key check and the reader's fixed-segment parse
  need nothing new.
- **`source` is a free string, not an enum**, so adding a provider is not a
  versioning event. `cond` is the opposite on purpose: closed, because the
  card needs an icon for every value.
- **No C reader.** The card's session has not asked for `kdash_weather()`.
  Adding one later is additive.
- **No publisher change.** `kdash` is already in both wrappers' `NAMESPACES`.

## What shipped

- `contracts/schemas/kdash-weather.schema.json` — three examples (an
  Open-Meteo record, an NWS afternoon record with nulls, a polar night) and
  five counterexamples (absent `hi_f`, provider text in `cond`, millisecond
  sun times, millisecond `ts`, out-of-range humidity).
- `contracts/registry.md` — the table row and a section for the feed.

## Evidence

`just check` green on kai, 2026-11-12: docs gate clean (schema registered,
examples accepted, counterexamples rejected), 60 Python tests, 79 Rust tests,
5 of 5 ctest. `kdash_pub.keys.check_key("kdash:weather:home")` accepts the
key and refuses a token with a space. **Nothing was written to Redis**: the
first real `kdash-pub set` is kweather WI 4574's, after this merges.

Before this sprint, on the same day, a
live NWS-sourced payload from the collector validated against the schema with
`scripts/jsonschema_mini.validate` (no errors). The Open-Meteo path had not
produced a live payload: its forecast API was unreachable all day.

## Repaired in passing

- `check-docs` requires the Status paragraph in `CLAUDE.md` and
  `.github/copilot-instructions.md` to name the newest sprint record. Both
  now mention sprint 021.

## Follow-ups

- kweather WI 4574 — install the timer on kai. Waits on this merging and on
  Ken supplying coordinates.
- kpidash — bump `lib/kdashdata` to this sprint's merge commit to get the
  schema in-tree (nothing in the C library changed).

## Deployed

2026-11-12, from merged `main` (`bf56d03`, PR #22), by `/sprint-ship` running
the two steps `.sprint-deploy` declares.

- **`just publish`** — a deliberate no-op: "nothing to publish: 0.1.0-4837473
  already in the store". This sprint changed no publisher input.
- **`just deploy`** — knarr installed `kdash-pub` `latest` (`0.1.0-4837473`,
  unchanged) on kai, kubs0, cleo, kimac and thrall. komarchy and kbook2 did
  not answer and thrall-arch did not resolve; all three are `--host-optional`
  and were reported SKIPPED, exit 0.

What this sprint delivered is a schema and a registry entry, and those are
live by being on `main`: `scripts/check.py` on merged `main` reports 20
schemas registered and validating. No key was written; the first
`kdash:weather:home` record is kweather WI 4574's.

Noted, not changed: thrall-arch is still in `just deploy`'s optional host
list although that install was retired on 2026-10-24 (korg:4034). It costs a
skipped line per deploy and nothing else.
