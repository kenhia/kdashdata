# Sprint 015 — Contracts said, and contracts enforced

Proposal: korg:2978 (slice 2 of program korg:2981, "Low-hanging fruit —
experiment 1"). Run as an overseen karc leg on kai.

Five covered items, in the order they were done: two contract notes that had
gone stale or stayed pending (2644, 2761), then the gate that makes a schema
mean something (1926), then the two read verbs a publisher needs (1929), then
the declaration that lets `/sprint-ship` publish and deploy this repo without
being asked (2798).

They share a theme, which is why they are one sprint: **this repo says things
about its feeds, and until now it said them only in prose.** A schema nothing
validates against, a registry number nothing compares to kpidash, a settled
decision still written as an open question, a deploy nothing declares. Each
one is a sentence with no gate behind it.

## Premise check at start

| item | verdict |
| --- | --- |
| 2644 | **holds.** `registry.md` said "60 s / 300 s windows in kpidash today" and the schema "60 s window in kpidash"; kmon's 26 h window has been live since kpidash sprint 018. |
| 2761 | **holds**, with the wording drifted. The notes do not literally say "not decided in this sprint" — they say the difference "is deliberate" and "(see the follow-up on that family)". Same defect: a reader cannot tell a ruling from an open question. |
| 1926 | **holds.** `grep -rn "jsonschema\|validate" scripts/ justfile` returns nothing, and no schema carries an `examples` block. |
| 1929 | **holds.** `READ_USAGE` has exactly one row, `hget`. No `get`, no `scan`, in either wrapper. |
| 2798 | **holds.** No `.sprint-deploy` in the repo; `just publish` republishes unconditionally. |

Cross-project plan check: `cross-project-planning/index.md` does not list
kdashdata. Nothing applies.

## 2644 — the freshness window is the consumer's, and it is per service

`contracts/registry.md` and `kpidash-service-status.schema.json` both quoted
kpidash's default window as if it were the contract's. It is not, and it has
not been since kpidash sprint 018 gave `kmon` a 26 h window — which matters,
because `kpidash:services:kmon:kai` is published once per nightly and a reader
of this contract would have expected it to go red after a minute.

Both texts now say the window is **consumer-side and per service**, name
kpidash's 60 s default and kmon's 26 h as the live exception, and point at
kpidash's `docs/CLIENT-PROTOCOL.md` §8a as the source rather than restating a
number that can change again. Both also record that the payload deliberately
carries **no cadence field** (handoff korg:2309) so the next reader does not
propose one.

## 2761 — a ruling written as a ruling

Ken decided on 2026-09-16 that `claude-session.schema.json`'s `ts` stays
unbounded, per CD-3: a grandfathered shape is documented as it is and migrated
opportunistically, never tightened underneath a writer this repo does not own
(`claude-pub.sh` is installed on kai, kubs0 and cleo). The 1e11 bound belongs
to new families, where it catches a new writer's unit slip before it publishes
— which is exactly `ghcp:session`'s case.

Both asymmetry notes now say **settled**, and why: the `ts` description in
`ghcp-session.schema.json`, and the ghcp section of `contracts/registry.md`.

A third place got the note too, and it was not in the item: **`claude-session.
schema.json`'s own `ts`**. That is where a reader looking at the unbounded
field actually is when the question occurs to them, and leaving the answer only
in the *other* family's schema is how it gets re-asked. Mechanical, same
sentence, no decision.

## 1926 — a schema that has never rejected anything is not a gate

**The decision (CD-24): option 2, and not the dependency.** `jsonschema` would
have been three lines of work and would catch everything. It was declined
because "this runs with nothing installed" is a property of `check-docs` and
`check-python`, and spending it to make a gate is what this repo's conventions
specifically forbid. So: `scripts/jsonschema_mini.py`, ~260 lines, sixteen
assertion keywords, importing nothing but `re`.

**Where the records live: in the schema file.** `examples` (standard
annotation, every entry must validate) and `x-counterexamples` (this repo's
own — `{"record": …, "why": "…"}`, every entry must be *rejected*, `why`
required). Next to the schema rather than in a test file, because the two
drift apart the moment they are in different directories, and because the
counterexamples turn out to be the best documentation the schema has. All
fifteen schemas now carry both: **27 examples and 55 counterexamples**, run on
every `just check`.

`kdash-stale.schema.json` carries the one this item was filed for: `since` as
`"2026-09-05T04:10:40Z"`, which is *literally* the value in k-homelab's
`.state/last-seen`. A writer reading the contract now meets the trap before
making it.

**The engine refuses what it cannot check.** `anyOf`, `$ref`, `format` — any
keyword outside its sixteen raises rather than being skipped. The walk that
enforces this (`check_keywords_deep`) is deliberately separate from validation,
because `validate` descends only into fields an example actually carries: a
rarely-filled property could otherwise grow an unimplemented keyword and go
unchecked until someone wrote an example for it. Proven below.

Two semantics are hand-written because they are where a hand-rolled validator
goes quietly wrong, and both have counterexamples in the corpus:

- **`true` is not `1`.** Python's `True == 1`, so `{"const": true}` would
  accept `1` and `{"enum": [0, 1]}` would accept `false` — and both shapes are
  live here (`kdash:stale`'s `stale`, `claude:limits`' `scoped_active`).
- **`$` means end of input.** JSON Schema patterns are ECMA-262 without `m`;
  Python's `$` also matches before a trailing newline. Measured: a naive
  `re.search("^[A-Za-z0-9._-]{1,63}$", "kai\n")` **matches**, so a host token
  with the classic shell-capture newline would have passed. It is rejected.

### Negative tests — each error planted, each seen to exit 1

| planted | gate said |
| --- | --- |
| an `examples[0]` made invalid (`reason: ""`) | `examples[0] should be valid but reason: shorter than minLength 1` |
| an `x-counterexamples[0]` made valid (`want: "dash"`) | `x-counterexamples[0] was ACCEPTED but must be rejected — …` |
| `anyOf` added to `selftest.note` | `properties.note: keyword(s) anyOf are not implemented …` |
| `oneOf` added to `telemetry.disks.items.properties.type` — three levels down, reached only through an example's disk row | `…disks.items.properties.type: keyword(s) oneOf are not implemented …` |
| `SCHEMA_DIR` pointed at an empty directory | `no *.schema.json found — this gate would pass vacuously` |
| all fifteen schemas with no `examples` (the state before this sprint) | fifteen errors, one per schema |

The control is the repo as it stands: green.

### Repaired in passing

`scripts/__pycache__/` — `check.py` now imports a sibling module, so running
the gate leaves a `__pycache__` the repo did not ignore. `.gitignore`'s
`publishers/python/**/__pycache__/` became a plain `__pycache__/`. Caused by
this change, one line, proved by a clean `git status`.
