# Sprint 018 — Fold the knarr workarounds back, list `availability`, decide how korg data reaches a dashboard

**Proposal:** korg:3233 (a slice of program korg:3245, "Low-hanging fruit — run 3")
**Covers:** WI 3094, WI 3179, WI 1803
**Branch:** `018-knarr-fold-back-availability-korg-feed`
**Run as:** karc leg `kdashdata-520bac` on kai, overseen

## Goal

Three unrelated items that each needed only a decision the evidence or the
overseer's brief had already made:

- **3094**: knarr 0.5 grew `--host-windows` (WI 1763) and `--host-optional`
  (WI 2680), the two features this repo's deploy recipes were standing in for.
  Fold both stand-ins back into one `deploy`.
- **3179**: kpidash 023 added an optional `availability: "intermittent"` to
  `kpidash:client:{host}:health`. List it in the registered schema.
- **1803**: record how korg data reaches a dashboard, before kstudiodash's
  Rate of Fire and Awaiting Ken widgets get built against raw HTTP.

## Premise check

Run from kai at 12:05 PDT.

- **3094 holds.** knarr on kai and kubs0 is `0.6.0-275e1fa`, and
  `knarr deploy --help` lists both `--host-windows` and `--host-optional`. The
  comment 2910 ruling is on the item. A fleet grep found no external caller of
  `deploy-cleo`, `deploy-komarchy` or `deploy-all`. The only mention outside
  this repo is a doc line in kdeskdash's `publisher/README.md:41`.
- **3179 holds, and one half of it changed.** The schema was
  `additionalProperties: true` with no `availability`. kdeskdash reads only
  `kpidash:client:*:dev_telemetry` (`src/telemetry_host.h`), never `:health`,
  so the "does kdeskdash honour it" half has nothing to honour. The consumer
  that *does* render host health through libkdash is **kstudiodash**
  (`src/feeds.c:83`).
- **1803 holds.** No `kdash:korg:*` exists in the registry. korg's `get_board`
  and `work_item_flow` are the rollups kfdc reads, and korg-dash WI 618's
  newest comment points at the same `get_board` read.

Cross-project plan: kdashdata is not listed in the plan index, so none applies.

## Decisions

- **One `deploy`, with no aliases.** `knarr deploy kdash-pub --host kai,kubs0
  --host-windows cleo --host-optional komarchy`. `deploy-cleo`,
  `deploy-komarchy` and `deploy-all` are gone, and so are `deploy-all`'s ssh
  probe and `scripts/install-cleo.ps1`. The daily SKIPPED line for a shut
  laptop is the ruled end state (comment 2910). `.sprint-deploy` now runs
  `recipe: deploy`.
- **The `.ps1` ASCII gate stays.** It now finds nothing to check. It exists
  for any script cleo runs, not for the one file it was written for. Its
  comment says so.
- **`availability` is listed as `enum: ["intermittent"]`, optional.** The new
  counterexample is a typo (`"sometimes"`). kpidash reads any other value as
  absent, so a typo draws a sleeping host red. The contract names that mistake.
  libkdash's `kdash_health_t` is unchanged. The registry note says a C
  consumer therefore sees an asleep host as offline.
- **CD-26: option 1.** A publisher reads korg's rollups and writes
  `kdash:korg:board` and `kdash:korg:rate_of_fire` to central, in kfdc's
  vocabulary. The option of a panel calling korg over HTTP is rejected on the
  record, and an HTTP client in libkdash loses on CD-9's grounds.
  **Two keys, one per korg read**, so each `ts` names one consistent snapshot.
  The stated defaults are a 60 s / 5 min cadence, a user timer on kubs0, and
  **korg-dash** as the owning repo. The brief said "korg-side", and korg-dash
  is a korg *reader*, which keeps Redis out of the system of record. This
  reading is flagged for the overseer's ruling. The registry rows are
  provisional and carry no schema until the first writer exists.

## What shipped

- `justfile`: one `deploy` recipe replaces four.
- `scripts/install-cleo.ps1`: deleted.
- `.sprint-deploy`: `recipe: deploy`.
- `docs/architecture.md`: CD-13's host table and komarchy section are updated
  for the fold-back, and CD-26 is new.
- `contracts/schemas/kpidash-client-health.schema.json`: gains
  `availability`, one example and one counterexample.
- `contracts/registry.md`: an `availability` note in the kpidash family, and a
  provisional `kdash:korg:*` subsection in the kdash family.
- `publishers/README.md`, `scripts/check.py` and `scripts/build-darwin.sh`:
  stale recipe references are updated.

## Gates

- `just check`: green (rc 0). All four gates passed, including 15 schemas validating their examples and counterexamples and the ctest suite at 5/5.
- Negative test for 3179: with the enum swapped for `"type": "string"`,
  `check-docs` reports `x-counterexamples[2] was ACCEPTED but must be
  rejected` and exits 1. It is green again once restored.
- `just deploy --dry-run` from kai planned kai, kubs0, cleo and komarchy, all
  `ok`, with rc 0. komarchy was awake, so the SKIPPED path was not exercised
  live. knarr's own sprint 005 measured it.

## Follow-ups (raised for the overseer, not filed)

- kdeskdash `publisher/README.md:41` still names `just deploy-cleo`. This is a
  one-line doc fix in another repo, and program korg:3245 has a kdeskdash slice
  (korg:3234) in wave 2.
- Whether libkdash should surface `availability` so kstudiodash can draw an
  asleep host. That needs a struct change here and a rendering choice there.
- Alignment with the kpolice sibling (WI 3092). That leg was still on its
  pre-change justfile when this one was written.
