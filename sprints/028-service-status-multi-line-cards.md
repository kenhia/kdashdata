# Sprint 028 — Service status: multi-line cards (`title`, `lines`, `span`)

**Proposal:** korg:4851 (slice 1 of program korg:4855)
**Covers:** WI 4846
**Branch:** `028-service-status-multi-line-cards`
**Run as:** karc leg `kdashdata-440b4b` on kai, overseen

## Goal

Ken wants status cards that carry several lines, starting with `argus`
(2026-11-20). He left the shape to the agent. The design was fixed on WI 4846
before the sprint: same key, additive optional fields, no magic string. This
sprint lands the contract and the C consumer code, so kpidash's reader
(korg:4853) and argus's publisher (korg:4854) can follow.

## Premise check

Run on kai, 2026-11-20 17:54 PST.

- **Holds.** `kpidash-service-status.schema.json` had
  `additionalProperties: true`, `text` required, and no `title`, `lines` or
  `span`. `kdash_service_t` had none of them either.
- "Whatever the Python and Rust publisher helpers validate": they validate the
  key grammar and the `ts` rule, and nothing service-specific. The key is
  unchanged, so neither wrapper needed a change.
- No guiding plan lists kdashdata.

## Decisions

Recorded as **CD-32** in `docs/architecture.md`. In short:

- Same key, additive. `text` stays required as the fallback.
- Coarse words, not a layout engine: S/M/L, L/C/R, `#rrggbb`.
- The border colour stays the reader's (`state` plus freshness).
- A malformed `lines` is dropped **whole**, and the record survives. This is
  rules.md's "malformed optional is absent", applied at the array.

Three calls the WI left to the implementation:

- **Bare string or object in one `items` schema.** `jsonschema_mini.py` has no
  `oneOf`, and adding one would grow the validator. An `items` schema with
  `type: ["string", "object"]` plus `pattern` (string-only) and
  `required`/`properties` (object-only) says the same thing in standard JSON
  Schema, because each keyword applies only to its own instance type. No new
  keyword was needed.
- **No `maxLength` on `t` or `title`.** `text` has none, and the C side
  truncates display strings to `KDASH_TEXT_MAX` (127 bytes), as it already
  does for `text`.
- A rejected record leaves the struct fully zeroed (`span` 0). A record that
  parses has `span` 1 unless it said 2. That keeps kdash_payload.h's
  "false leaves a fully-zeroed struct" rule true for this parser.

## What shipped

- `contracts/schemas/kpidash-service-status.schema.json`: `title`, `lines`,
  `span`. Two new examples: the argus layout with every field, and an empty
  `lines`. Seven new counterexamples: nine lines, `size: "XL"`,
  `color: "red"`, a line object with no `t`, a newline inside a line,
  `span: 3`, and `lines` without `text`.
- `include/kdash/kdash_payload.h`: `kdash_service_line_t`, the size and align
  enums (zero is the default), `KDASH_SVC_LINES_MAX`, and `has_title`, `title`,
  `span`, `n_lines` and `lines` on `kdash_service_t`.
- `src/kdash_payload.c`: `kdash_parse_service()` fills them.
- `tests/test_payload.c`: a new block. It covers the defaults, a full
  round-trip, exactly eight lines, an empty array, eleven malformed `lines`
  shapes, four bad `span` values, two bad titles, and zeroing on rejection.
  Each malformed shape has to keep the record, drop every line, and leave the
  other fields intact.
- `examples/kdash_dump.c`: prints the title, span and lines when present.
- `contracts/registry.md`: a "Multi-line cards" note under the kpidash table.
- `docs/architecture.md`: CD-32.

## Evidence

- `just check` is green: docs, python, rust and all 5 ctest suites.
- `just build-aarch64` builds clean.
- **Negative tests.** Raising the schema's `maxItems` to 9 made `check-docs`
  fail on the nine-lines counterexample. Removing the parser's reset-on-failure
  made `test_payload` fail "bad lines N dropped whole" for every shape with a
  good line in front of the bad one. Both changes were reverted.
- The existing service tests are unchanged and still pass.
- Not run live: publishing a demo card to central would put a card on
  kpidash's live panel. The acceptance criteria are parser-level, and kpidash's
  own parser does not read these fields until korg:4853.

## Repaired in passing

None.

## Follow-ups

- kpidash pins this repo as `lib/kdashdata`. The next kpidash slice
  (korg:4853) starts by moving that pin to this sprint's merge commit.

## Deployed

2026-11-20 ~18:00 PST, from merged `main` (`d3f2d09`, PR #29), by the steps
`.sprint-deploy` declares, run from kai:

- `just publish`: **deliberate no-op.** "nothing to publish: 0.1.0-4837473
  already in the store". This sprint changed no publisher input (schema, C
  library and docs only).
- `just deploy`: exit 0. `kdash-pub 0.1.0-4837473` (unchanged) was confirmed
  on kai, kubs0, cleo, kimac and thrall. komarchy and kbook2 did not answer
  ssh (connection timed out), and thrall-arch did not resolve. All three are
  `--host-optional` and were reported SKIPPED, which is the normal outcome
  for a sleeping host.

Nothing live reads the new fields yet. The first reader is kpidash's service
card (korg:4853), whose `lib/kdashdata` pin moves to `d3f2d09`.
