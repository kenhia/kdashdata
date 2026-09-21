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

## 1929 — `get` and `scan`, and absence as an answer

CD-14 said a second read verb "needs a reason of the same shape: a publisher
that cannot write correctly without it". `kdash:stale:{host}:{deployer}`
supplied two. It is a **read-modify-write** — `since` must be carried unchanged
across every later skip, and a writer that restamps it turns "stale for three
weeks" into "stale for an hour", which looks exactly like working code — and
its identities are in the **key**, so "which deployers have skipped this host"
is a question about key names.

So: `get <key>` and `scan <pattern>`, in the Rust CLI **and** the Python
wrapper. Python gets them on the evidence CD-14 asked for: kmon's nightly
(korg:2213) reads `kdash:stale:{host}:*` and is Python.

### Absence is a distinct answer, and it drove the exit codes

`hget` folds "absent field" and "absent key" into one empty answer, because
Redis does and the `claude:limits` guard does not care. `get` cannot:
`kdash:stale` is presence-owned (CD-18) and **absence is the only all-clear**,
so a caller that cannot tell "not set" from "set to empty" cannot implement the
feed. Hence a deliberate divergence, recorded as an amendment to CD-14:

| code | `get` | `scan` | `hget` (unchanged) |
|---|---|---|---|
| 0 | present — value on stdout | answered, possibly no keys | present, or absent with no output |
| 1 | **absent** | (not used) | the command was wrong |
| 2 | could not ask — incl. a refused command | same | delivery failed |

`hget` has no code meaning "absent", so 1 is free there for a bad command.
`get` needs 1, so a refused command moves to 2 — where it is true, because no
answer came back either way. These are `just published`'s three outcomes on
purpose (present / absent / could-not-ask), so one idiom covers both.

**`--best-effort` does not touch `get` or `scan`.** Folding 2 into 0 would
spell "I could not ask" exactly like "here is the answer" — and a caller that
read an unreachable Redis as "absent" would restamp `since`. `hget` and every
write keep the flag unchanged.

### A pattern is not a key

`check_pattern` is its own function on both sides rather than a flag on
`check_key`, and the reason is one rule: **the namespace segment may not be
globbed.** Relaxing `check_key` to let `*` through anywhere would have
legalised `*:*` — one argument that reads every family on a Redis shared with
kvscf and the dashboards. Redis's `[abc]` classes and `\` escapes are refused
too. `scan` uses SCAN cursor-to-cursor (never KEYS, which blocks the server for
the whole sweep), deduplicated and sorted.

### Acceptance — live, against the central Redis from kai

The gates are pure code by design, so the read path was exercised for real
against `rpi53:6379` (auth from `/etc/khomelab/secrets.env`, CD-19), using
`kdash:selftest:kai` — the key that exists to be a canary — and cleaned up
after.

| case | result |
| --- | --- |
| `get` an absent key | rc 1, nothing on stdout |
| `get` a present key | rc 0, the value |
| **`get` a present-but-EMPTY key** | rc 0, stdout exactly `0a` — one newline |
| the same key after `del` | rc 1, **zero bytes** |
| `scan 'kdash:selftest:*'` | rc 0, the one key |
| `scan` matching nothing | rc 0, no output |
| `scan '*:*'` / `get weather:now` / `get 'kdash:stale:*'` | rc 2 each, each naming why |
| `--best-effort` against a dead endpoint: `get` / `scan` | rc **2** — not masked |
| `--best-effort` against a dead endpoint: `hget` / `del` | rc **0** — unchanged |

The empty-vs-absent pair is the one WI 1929 was filed on, and it is
distinguishable twice over: by exit code, and by stdout being one byte versus
none.

Unit tests: 75 Rust (up from 71) and 59 Python (up from 55). Negative-tested —
the namespace-glob rule was disabled on each side in turn and the matching test
failed on both.

### Not done here, deliberately

Removing the `kdash_auth` copies in agent-skills, k-homelab and (soon) kmon.
Those are other repos' contracts; k-homelab WI 2546 already covers its half.
These verbs are what makes that cleanup possible, not part of shipping them —
the proposal's notes say the same.

## 2798 — the repo finally says it deploys

kdashdata publishes `kdash-pub` to the package store and installs it on four
hosts, and declared nothing — so `/sprint-ship`'s Phase 7 was skipped silently
on every ship. Sprints 010, 011 and 013 all deployed because a human or an
overseer asked in that session, never because the repo said so.

The item was Branch B when it was filed, and the decisions it was waiting on
have since been made. **Ken's 2026-09-17 "one rule" on klaude-top WI 2782 and
kdeskdash WI 2801 is extended here by analogy** — stated plainly because it is
an extension, not a quotation: the version comes from the **last commit
touching the artifact's inputs**, not `HEAD`.

### The four questions the item asked, answered

**1. What counts as a change?** Path-based, and narrower than the item's own
sketch: `publishers/rust/src`, `build.rs`, `Cargo.toml`, `Cargo.lock`.
`publishers/python/**` is deliberately **out** — it is a separate wheel with a
separate publish step, and changing it leaves this binary identical, so
including it would republish a shared binary to four hosts for a change that
is not in it. The READMEs and `tests/` are out for the same reason. See the
flag for the overseer below.

**2. What does a no-op print, and what does it exit?**
`nothing to publish: <version> already in the store`, exit 0 — the same
sentence klaude-top prints, so the three recipes read alike.

**3. Does `deploy-all` run when `publish` no-ops?** **Yes, always.** A host can
be behind `latest` without this sprint changing anything — komarchy was ten
days behind in sprint 011 — so "publish no-op'd, therefore skip the deploy" is
not right. It installs `latest`, unchanged in that case, and knarr's confirm
step still reports what each host runs.

**4. komarchy inside an automated ship.** A normal skip. `deploy-all` already
probes it **from the host doing the deploying**, prints the skip, and exits 0;
nothing about that needed changing.

### The store predicate refuses to guess

Three outcomes, and the remote answers with a **word** rather than an exit
code: a downed store host, a missing host key and a genuinely absent version
all make `ssh … test -d` exit non-zero, and reading any of them as "absent"
would republish over a store nobody could see — a false claim about the world,
not a failed command. `publish` treats could-not-ask as a refusal.

### One fact, derived twice, and the assertion that keeps it one

`just version` reassembles the label from git so the predicate can run
**without a cross-compile** — a no-op nobody would leave declared if it cost a
two-target build. `build.rs` derives it again inside the binary, because
knarr's confirm step re-reads the *installed* binary's `--version`. Two
derivations of one fact is exactly the drift the old comment in `version`
warned about, so `publish` now **compares them and refuses** if they disagree,
naming both places to reconcile. That is stronger than the assumption it
replaces: it checks rather than trusts.

### Acceptance

| check | result |
| --- | --- |
| `just version` is input-scoped | `0.1.0-a044678` — moves with an input commit, not with `HEAD` (proved below) |
| `just published <stored version>` | `present: … is in the store`, exit **0** |
| `just published <this branch's version>` | `absent: … is not in the store`, exit **1** |
| `just published` with an unresolvable store host | `cannot reach … Name or service not known`, exit **2** |
| `just publish` from a dirty tree | refuses, exit 1 |
| `just publish --bad-arg` | rejects, exit 2 |
| `just publish --dry-run` | names the version and the `--no-latest` branch guard, touches nothing |
| `just publish` where the version is already stored | `nothing to publish: … already in the store`, exit **0** |
| `just check` | green — all four gates |

The double run from merged `main` is left to the ship's Phase 7 **by design**:
that is a **trigger, not a soak** — the ship fires it, in the same session,
and it is the end-to-end test of this sprint's own deliverable. No soak work
item was created.

### Repaired in passing

A backtick inside a double-quoted `echo` in the new drift message would have
run `just version` as a command substitution every time the error printed.
Caught before it shipped; the message is plain words now.
