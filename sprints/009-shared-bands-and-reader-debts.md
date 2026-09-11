# 009 — Shared temperature bands, the counted-reader contract, and two doc debts

korg: proposal 2217, work items 1786, 1790, 1804, 1928, 1935. Slice 3 of the
backlog-drain program korg:2233, run as an overseen sprint in karc leg
`kdashdata-405fa1` on kai. Overseen: an overseer session reviews before and
after the ship, and the ship waits on its green light.

## Goal

A backlog-drain sweep rather than a themed sprint: five items that had been
sitting long enough that the first job was finding out whether they were still
true. One was not. The other four were, and they divide neatly into one real
behaviour change (the counted readers), one piece of shared data model (the
apartment-temperature bands), and two pieces of documentation that had stopped
describing the repo.

## Premise check at the brief

Four of five held. The fifth had been done and nobody noticed.

- **#1786 — GONE.** "Typed readers for the `claude:*` family" was filed
  2026-09-02. Sprint 006 (`f95a233`, korg:1784) shipped it in full two days
  later: the key grammar, the three record types, the HASH field/value parsers,
  the CD-16 display ladder, all three readers, and the stem-selecting resolve.
  Every bullet of the item exists in the tree, and sprint 006's own record
  verified all three feeds live against `rpi53:6379`. Set `done` with that
  evidence rather than worked.

  This mattered beyond one item. The proposal's premise — "kdeskdash 1783 is
  blocked on 1786" — and the program's leg ordering ("slice 3 unblocks slice
  4") were both built on it. **kdeskdash's adoption slice (korg:2218) was never
  blocked on this sprint**, and that was surfaced to the overseer at the brief
  rather than discovered at ship time.

  How it hid: #1790 is a *follow-up* to #1786's landed implementation — sprint
  006's record lists it under Follow-ups, explicitly not covered there. The two
  items bundled together read as a feature plus a known wrinkle in it, which is
  exactly what a feature that has not shipped yet looks like.

- **#1790 — holds, and is wider than filed.** The item named
  `kdash_claude_sessions()`. All three counted readers carry the identical
  code: `kdash_services()`, `kdash_apttemps()` and `kdash_claude_sessions()`
  each did `if (st == KDASH_UNAVAIL) return n > 0 ? n : -1;`. The item
  anticipated this — "changing one means changing all three" — but its
  suggested resolution ("pick whichever the kpidash SCAN readers already do")
  had no answer, because all three do the same wrong thing. So the rule had to
  be decided, not copied.

- **#1804 — holds, refined.** kpidash owns the thresholds
  (`src/registry.c:238`); kstudiodash ported them verbatim
  (`src/board.c:543`, with a comment saying so). The item asked whether
  kdeskdash carries a third copy: **it does not** — kdeskdash has no
  apartment-temperature code at all. Two copies, not three.

- **#1928 — holds, drifted the same direction.** The title says four sprints
  behind; sprint 008 shipped after it was filed, so it was five (004–008).

- **#1935 — holds.** CD-18's text covered the payload only.

No cross-project plan applies — kdashdata is not in the `cross-project-planning`
routing table.

## Decisions

### A counted reader returns -1 or a complete list (#1790)

The item offered two resolutions and said consistency mattered more than which:
write `*skipped` on the early-return path and document that a count returned
while `kdash_conn_reachable()` is false is partial; or return -1 outright.

**Chose -1.** Three reasons, in order of weight:

1. **A partial list is a confident wrong answer.** These feeds render missing
   rows as absent *things* — services that are not running, zones whose sensor
   died, Claude sessions that ended. A dashboard showing three of eight
   services with no other signal is worse than one showing "unavailable", and
   the cost of the honest answer is a single tick: CD-6's lazy connect
   reconnects on the next one. This is CD-18's argument in a different dress —
   a reader that cannot see all of a feed must not render the gap as an
   all-clear.
2. **The header already promised it.** `kdash_feed.h` has said "Returns the
   count written, or -1 when unreachable" since sprint 002. The code
   contradicted its own documented contract, so this is the cheaper repair:
   change the code to match the doc, not the doc to match the bug.
3. **The alternative puts a new obligation on every caller.** Option one is
   only safe if every consumer starts calling `kdash_conn_reachable()` after
   every read. kpidash has already shipped without doing that, so option one
   would leave the bug live in the one consumer that exists while declaring it
   fixed.

The `*skipped` half of the item resolves itself under this rule: `*skipped` is
already zeroed at each function's head and nothing writes it before the failure
point, so **0 becomes the correct answer** rather than a misleading one, and
the contract is "on -1, neither `out` nor `*skipped` carries information."

`kdash_clients()` and `kdash_claude_recent()` are single round trips and cannot
fail this way. The rule is stated once in `kdash_feed.h` over all three counted
readers rather than three times.

### The apartment-temperature bands move here (#1804)

Straight application of CD-16 — derivation in, formatting out — so this mints
no new decision. `kdash_apttemps_band(temp_f, stale, cold_f, ok_f, hot_f)` in
`kdash_payload.h`, defaults `KDASH_APTTEMPS_{COLD,OK,HOT}_F` in
`kdash_freshness.h` beside the family's window, exactly the shape
`kdash_ladder()` and the claude derivation already use. Which colour a band
renders as stays in the panel (CD-10); the library returns a vocabulary.

Two calls worth recording:

- **`stale` wins over the number.** A reading nobody has refreshed in 300 s is
  not evidence about the room, and both panels already grey it. Making that
  ordering the library's rather than each panel's is the point of the move.
- **The boundaries reproduce kpidash's `< 65 / <= 75 / < 80 / else` exactly**,
  including that 75.0 is `ok` and 80.0 is `hot`. The move is only worth making
  if the two panels keep agreeing, so every boundary is asserted on both sides.

### One clamp, not two — and the negative test is why

`kdash_ladder()` clamps out-of-order thresholds so a caller cannot configure an
unreachable state, and the first draft here did the symmetrical thing:

```c
if (ok_f < cold_f) ok_f = cold_f;
if (hot_f < ok_f)  hot_f = ok_f;   /* removed */
```

Planting the fault revealed the second clamp **cannot change any answer**: the
`temp_f <= ok_f` test runs first and has already claimed everything a collapsed
warm band could have held. It was untestable because it was unobservable, and
it was removed rather than left as a branch no test could reach. The comment in
`kdash_payload.c` says so, because the symmetry is tempting enough that someone
will add it back.

The same negative test caught that the *first* draft of the clamp tests was
vacuous — both cases returned the same band with or without clamping. The case
that distinguishes them is `temp_f == cold_f` with `ok_f` below `cold_f`:
clamped it is `ok`, unclamped it falls through to `warm`.

### The stale status line gets a gate, not just a fix (#1928)

#1928 suggested it and was right: writing the line is judgement, but noticing
it has gone stale is one comparison. `scripts/check.py` now fails if CLAUDE.md's
`Status:` paragraph does not name the newest `sprints/` record. A sprint that
adds `sprints/NNN-*.md` and does not touch the status line now fails
`just check` in the sprint that caused it, which is the only time the fix is
cheap. Four sprints of silent rot is the failure mode it removes.

### CD-18 extends to key shape (#1935)

Upheld as an extension rather than a reading, and filed into the contract
because kpidash sprint 017 had already implemented it and nothing but its own
source recorded the conclusion. An off-contract *key* is more dangerous than an
off-contract payload: a dropped payload still raises its host, a dropped key
renders nothing at all. The generalised rule — the inversion covers anything
the reader cannot fully understand about a presence-owned record, payload and
key alike, and stops only where the record cannot be attributed to a subject —
is now in CD-18 with the three-row table kpidash implements, including the
honest boundary that `kdash:stale:` with no host is ignored.

The heading changed with it ("a bad payload never clears it" → "nothing the
reader cannot parse — payload or key — ever clears it"). Safe: every reference
to CD-18 in this repo and in kpidash, kdeskdash and kstudiodash cites it by id,
not by anchor.

## What shipped

**Behaviour** (`src/kdash_feed.c`): the mid-list `KDASH_UNAVAIL` path in all
three counted readers now zeroes `out` and returns -1. Contract stated once in
`include/kdash/kdash_feed.h` under a new "the counted readers" heading, and in
`contracts/registry.md`.

**Pure core**: `kdash_apttemps_band()` and `kdash_temp_band_label()` in
`kdash_payload.h/.c`; `KDASH_APTTEMPS_{COLD,OK,HOT}_F` in `kdash_freshness.h`;
17 new checks in `tests/test_payload.c`.

**Docs**: CD-18 extended and re-titled; `contracts/registry.md` gains a bands
note and a counted-reader note; `CLAUDE.md`'s status line brought from sprint
003 to 009.

**Gate**: `scripts/check.py` grows the status-line check. `README.md`'s
description of `check-docs` was stale in both directions — it had never
mentioned the `.ps1` ASCII check from sprint 004 either — and now lists all
four.

## Negative tests

Every gate was seen to fail before it was believed.

| Planted fault | Caught by |
|---|---|
| 75.0 boundary exclusive rather than inclusive | `test_payload` |
| `stale` stops beating the number | `test_payload`, 2 checks |
| The surviving clamp removed | `test_payload` (after the test was fixed — see above) |
| The symmetrical second clamp removed | **nothing** — which is how it was found to be dead and deleted |
| CLAUDE.md's status paragraph stops at 007 | `check-docs`, exit 1 |
| CLAUDE.md's `Status:` line renamed away | `check-docs`, exit 1 |

**#1790's fix has no unit test, and knowingly so.** `kdash_feed.c` is the I/O
shell, which CD-10 keeps out of `just check` — there is no fake Redis and
`tests/` links only `kdash_core` for the pure suites. Triggering a *mid-scan*
endpoint drop would mean killing a live fleet Redis between a SCAN and a
HGETALL, which is not a test worth having at that price. What was verified is
that the readers still work: see below. The gap itself is filed rather than
shrugged at (WI 2246).

## Live verification

`just dump` against `rpi53:6379`, auth from the CD-12 env file
(`~/.config/kpidash-client/redis-auth.env`). Both stems resolved and connected
separately; all seven feeds read with **zero skips**:

```
KDASH_CENTRAL_REDIS  rpi53:6379 (connected)
KDASH_CLAUDE_REDIS   rpi53:6379 (connected)
  6 host(s) ... 2 card(s), 0 skipped
== apartment temperatures (kpidash:apttemps:*) ==
  Bedroom  72.6 F  56% RH        <- ok
  Living   71.5 F  55% RH        <- ok
  Kitchen  70.4 F  57% RH        <- ok
  3 zone(s), 0 skipped
  12 session(s), 0 skipped
```

All three live zones classify `ok` under the new bands, which matches what both
panels render today. The leg's own session appears in the claude feed as
`working kai kdashdata "overseen 2217"` — the readers touched by #1790 are the
ones that produced this output.

## Follow-ups

- **kpidash and kstudiodash repoint onto the shared classifier** — WI 2244
  (kpidash) and WI 2245 (kstudiodash). Filed rather than done here: the
  proposal scoped this sprint to moving the thresholds, with the consumers as
  separate work. kstudiodash consumes kdashdata as a git submodule
  (`lib/kdashdata`), so its repoint is a submodule bump plus a call swap;
  kpidash vendors differently and its item says so. **Until both land the
  thresholds exist in three places, not one** — this sprint added the shared
  home, it did not yet remove the copies.
- **The counted readers have no fault-injection harness** — WI 2246. Three
  readers now share a contract whose failure path nothing can exercise.
- **korg:2218 (kdeskdash adopts the `claude:*` readers) is unblocked** and was
  never blocked by this sprint. See the premise check.
