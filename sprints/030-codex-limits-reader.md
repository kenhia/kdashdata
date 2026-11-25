# Sprint 030 — libkdash readers for `codex:limits` and bar `provider`

Cross-repo slice of kpidash proposal korg:4950 (WI 4941), program korg:4951.
Branch `030-codex-limits-reader`. Run from the kpidash karc leg on kai, which
needs both readers: WI 4941 asks for `codex:limits` to be parsed "in libkdash
beside `kdash_parse_claude_limits`", and kpidash's Agents card reads bars
through `kdash_parse_agents()`, which dropped `provider` on the floor (CD-31
addendum: "the existing C reader ignores the field").

## Premise check

Holds. Sprint 029 contracted both and wrote no C reader for either. Live on
central at 2026-11-25 13:50 PST: `codex:limits` carries a Pro record
(`five_hour_none=1`, `seven_day_pct=1`, `source=app-server`), and
`kdash:agents` has 55 bars, 8 of them `provider: codex`.

## Decisions

- **Every schema-required field is required by the reader.** `claude:limits`'s
  reader requires only its two percentages because that hash predates its own
  contract; `codex:limits` was born v1 with a stated required set, and a
  reader that defaulted a missing `five_hour_none` would be guessing what
  CD-33 makes the writer state. A rejected record is `valid == false`, never
  "no window".
- `*_resets_at` accept 0 (the schema's "unknown"), so they need a
  non-negative parser beside the strictly positive `field_ts()`.
- `kdash_codex_limits_stale()` is `kdash_cadence_stale()` on `updated_at`,
  not the claude family's legacy-window rule: this writer always states its
  cadence.
- `provider` is an enum with OTHER = 0, so a zeroed bar reads as unknown and
  any absent, non-string or unrecognized value is OTHER (the addendum's rule).

## What shipped

- `include/kdash/kdash_payload.h`, `src/kdash_payload.c`:
  `kdash_codex_limits_t`, `kdash_parse_codex_limits()`,
  `kdash_codex_limits_stale()`; `kdash_agent_provider_t`,
  `kdash_agent_bar_t.provider`, `kdash_agent_provider_from_str()` / `_str()`.
- `tests/test_payload.c`: both schema examples, each required field dropped
  in turn, the schema's counterexamples and other malformed values, the
  leftover five-hour rule, staleness at the boundary; provider present,
  absent and unrecognized.
- Registry and CD-31/CD-33 text updated; the registry's writer cell for
  `codex:limits` no longer says "planned".

No publisher change, so `publish` self-skips.
