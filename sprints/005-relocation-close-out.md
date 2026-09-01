# 005 — Relocation close-out: retire old home, registry flip, claude schemas

korg: proposal 1754, work item 1750; gate work item 1769 (`kdash-pub` read
verb), pulled in here. Slice 3 and the last of the claude-feed relocation
program (korg 1755). Overseen: an overseer session reviews before and after
the ship, and the ship waits on its green light.

## Goal

Finish CD-7. Slices 1–3 built the plumbing, distributed it and cut the
publishers over to a dual-write window with both panels reading central. What
remained was the half that makes the move **auditable**: schemas for the family
that just moved, a registry that says where it lives, the architecture decision
closed out, and then the old home's `claude:*` keys actually deleted.

The program's aim is met when the registry shows `claude:*` live-on-central and
`rpidash2:6380` serves only `kvscf:*`.

## Premise check at the brief

Everything in #1750 was measured live rather than read, because the whole
point of a close-out slice is that it is the checkpoint where the program
pauses if something went sideways. Nothing had.

- **Dual-write is real.** Both homes carried a byte-identical `claude:limits`
  (`updated_at=1788288254`, `source=oauth`, `host=kai`) and the same three
  session hashes — including this sprint's own session, which is the same
  proof slice 2 used and is still the strongest available.
- **CD-8 holds, and now holds by itself.** Four `kvscf:*` keys on
  `rpidash2:6380`, zero on central. More importantly both panels pin
  `KDESKDASH_KVSCF_REDIS_HOST/PORT=127.0.0.1:6380` explicitly since kdeskdash
  sprint 031, so the pin no longer depends on where the claude endpoint points.
- **`KDASH_CLAUDE_REDIS` is still one line** at `khlenv/store.yml:39` in
  k-homelab, answering `rpidash2:6380`.
- **No claude schemas existed.** Six schema files, none of them `claude:*`.
- **The registry had drifted further than the item said.** Besides naming the
  interim home, it still claimed `claude-pub.sh` "carries a hardcoded
  `192.168.1.144:6380` and speaks RESP over `/dev/tcp` with no AUTH" — true
  when written, made false by sprint 031.
- **`kdash-pub` had seven write verbs and no read.** The gate, #1769.

**The credential-fallback sweep the overseer asked for is done and clean.**
The generalised lesson from slice 2's `config.c` bug is that anything defaulting
one endpoint's *credential* to another's is fine until the endpoints part
company. Searched all eight kaed roots across the fleet (43k files) for
`*_REDISCLI_AUTH` and for `claude:(limits|recent|session)` consumers:

- The per-feed credential-variable pattern exists **only** in kdeskdash, and
  its one inherit-across-endpoints fallback is the bug already fixed there —
  the auth now follows the endpoint, inherited only when kvscf resolves to the
  same `host:port`.
- **No other consumer of `claude:*` exists on the fleet.**
- kdashdata's own path cannot have the fault by construction: `auth.rs` reads
  one variable with one 0600-file delivery route and an explicit `--no-auth`
  opt-out. There is no second feed to inherit from.

Nothing to file. Recorded because a sweep that found nothing is only worth
anything if it says where it looked.

## Decisions

### The gate: build the read verb, don't delete the guard

#1769 offered two exits and named the second a decision rather than a default.
Ken chose the verb. The guard being removed is what stops a poll writer
publishing a file-source observation up to 900 s old over a live statusline's
seconds-old one — gauges stepping backwards, and `expected_refresh_s` flipping
between 60 and 300 arbitrarily. kdeskdash's own
`independent-writers-need-independent-stamps.md` exists because of that class
of bug; spending it to save one deploy cycle was the wrong trade.

### CD-14 — one read verb, and it is a publisher's

The crate's stated boundary was "no reading". That was right about
*consumption* and stays right: the data model, the freshness ladder and the
skip-a-bad-record discipline live in `libkdash`, and a second copy in Rust
would be a second contract. But a publisher guarding its own write is not a
consumer, and the alternative was re-hand-rolling AUTH in bash — including the
refuse-a-group-readable-file rule — which is exactly what CD-11 built the CLI
to stop.

So the boundary narrowed rather than vanished, and
[architecture.md](../docs/architecture.md) says where it now sits: one field,
no model, the same key grammar, the same exit codes, not a consumer API.

Three shape calls inside that:

1. **`Query` is not `Command`.** Every `Command` variant goes into `pipeline()`
   with its reply ignored — a read is neither a write nor pipelineable that
   way. `READ_USAGE` therefore sits beside `USAGE` rather than as a row in it,
   which keeps the existing test that `USAGE` covers exactly what `parse`
   accepts true, and lets a matching test say the same for reads. No verb may
   be in both tables; that is asserted.
2. **A read verb in a write batch says so.** `kdash-pub` refuses a whole batch
   when any line is off-contract, so a stray `hget` would take a session's
   `DEL` down with it — under the message "unknown command", sending you after
   a typo that is not there. It gets its own error.
3. **Bytes, not `String`.** Decoding in the CLI would report a non-UTF-8 value
   as a *delivery* failure, which is the wrong thing to say about a Redis that
   answered perfectly well.

### Schemas describe the decoded record

`claude:session:*` and `claude:limits` are HASHes — the first non-JSON feeds in
this registry. Every value arrives off the wire as a string, so a schema could
honestly type them all `string` with patterns. It would also be useless to the
consumer that wants to validate what it parsed, and would read nothing like the
six schemas already here. They describe the **decoded** record and say so in the
first line of each `description`.

Required sets are the reader's actual acceptance contract, not a wish list:
`status` + `ts` for a session (the resurrection-race guard), `five_hour_pct` +
`seven_day_pct` for limits. Everything else is optional because the reader
genuinely tolerates its absence — `started_ts` is missing from a live session
on central right now, and renders as an unknown duration rather than a zero one.

## What shipped

- **`publishers/rust/`** — `Query` + `parse_query` + `READ_USAGE` +
  `is_read_verb` in `command.rs`, `Connection::read_field` in `lib.rs`,
  `Action::Read` and its wiring in the CLI, `ParseError::ReadVerbInWriteContext`.
  Five new unit tests; 48 total, `fmt` and `clippy -D warnings` clean.
- **`contracts/schemas/claude-{session,limits,recent}.schema.json`** — the
  family that just moved, schema'd as CD-7 said it would be.
- **`contracts/registry.md`** — claude family flipped to *central, live*, with
  the three schema links; the interim home gone from the Homes table; the
  stale hardcoded-IP paragraph replaced; the kvscf section corrected now that
  its endpoint no longer defaults to the claude one.
- **`docs/architecture.md`** — CD-7 closed out with the program's four slices
  and the three things it learned that outlive it; CD-14 added; CD-1, CD-4 and
  a dangling `OQ-1` reference brought in line with a completed relocation.
- **`publishers/rust/README.md`** — the `hget` section, and `--no-auth`
  re-described now that no home in the registry is unauthenticated.

## Negative tests

The read verb was watched failing on every branch, live against both homes,
before anything depended on it:

| case | result |
|---|---|
| `--stem KDASH_CLAUDE_REDIS --no-auth hget claude:limits updated_at` | `1788288974`, exit 0 |
| `--stem KDASH_CENTRAL_REDIS hget claude:limits updated_at` | `1788288974`, exit 0 |
| absent field | empty stdout, exit 0 |
| absent key | empty stdout, exit 0 |
| off-contract key under `--best-effort` | exit **1** — not swallowed |
| unreachable endpoint | exit 2; exit 0 with `--best-effort` |

The fifth row is the one that matters: `--best-effort` exists so a dead Redis
cannot fail a hook, and it must never hide a key that violates the grammar.

## The retirement sequence

This repo's merge is the *start* of the close-out, not the end of it. The order
is not arbitrary — every step's failure mode is "the new state is empty", never
"both states are gone":

1. `just publish` + `just deploy-all` from merged `main` — the read verb reaches
   kai, kubs0 and cleo **before** anything calls it.
2. kdeskdash: `KDD_LEGS=claude` as the shipped default, the `interim` arm
   deleted, and the poll guard's read moved onto `kdash-pub hget`. Then
   `just publish-publisher` from its merged `main`.
3. k-homelab: flip `KDASH_CLAUDE_REDIS` to `rpi53:6379` in `khlenv/store.yml`.
   One line, which was the whole promise of the program.
4. k-homelab: bump `kdeskdash_publisher_version` in `manifests/common.yml` and
   `bin/apply` on kai **and** kubs0, `bin/audit` clean on both. That pin is
   deliberate, not a floor — until it moves, the next apply reverts both hosts
   and silently stops the publisher. cleo by hand, unmanaged.
5. Verify both panels on central, then `DEL` the `claude:*` keys on
   `rpidash2:6380` — and re-check that `kvscf:*` is still there afterwards.
6. klams: supersede the two records the overseer named, and record the end
   state.

**Steps 3 and 4 are in that order for a measured reason, and this is a
correction** — the sequence as programmed (and as first written here) had the
pin bump before the stem flip, which would have killed the feed silently. The
2.1.0 publisher writes one authenticated leg on `KDASH_CLAUDE_REDIS`; while that
stem still named the unauthenticated interim home, every write and the guard's
read failed `AUTH called without any password configured` — an *error*, not a
no-op — and `--best-effort` exists precisely to swallow that so a hook never
fails. Nothing would have reported it. Both directions were run against the live
endpoints before committing to either; the order above is the benign one, where
the old two-leg publisher merely loses its no-auth leg to `NOAUTH` and keeps
writing through central until the pin lands. The warning now lives in both
k-homelab files rather than only here.

**Rollback stops being neutral after step 5.** The 2.0.0 bundles still write the
interim leg and the 1.0.0 bundle writes *only* the interim home, so a pin
rollback after the DEL restores a publisher pointed at a dead feed. If a
rollback is ever needed past that point it goes forward to a fixed version,
never backward to an old pin.

Expect the claude panel to look emptier than reality for a few minutes at the
flip: a session already running writes a keepalive that carries only `ts`, by
design, and the parser correctly declines to render a hash with no `status`. It
heals at that session's next turn.

## Deployed

**2026-09-01**, from merged `main` (`7fe2c87`), in the corrected order above.
The overseer's clearance (korg:1754 comment 1222) covered the whole sequence
through the `DEL` and the klams pass.

**1. `kdash-pub 0.1.0-7fe2c87`** — `just publish` (linux + windows, one version,
`latest` moved), then `just deploy-all`. Verified by **naming** the hosts, and
by proving the new verb rather than that a file landed:

| host | path | `--version` | `hget claude:limits updated_at` |
|---|---|---|---|
| kai | `/usr/local/bin/kdash-pub` | `0.1.0-7fe2c87 (2026-09-01)` | `1788290414`, exit 0 |
| kubs0 | `/usr/local/bin/kdash-pub` | same | `1788290414`, exit 0 |
| cleo | `C:\tools\bin\kdash-pub.exe` | same | `1788290414`, exit 0 |

**2. kdeskdash `2.1.0-581799b`** — PR #39, squash `581799b`, `just
publish-publisher` from main. `KDD_LEGS` defaults to `claude`, the `interim` arm
deleted, the poll guard on `kdash-pub hget`. Its own record:
`kdeskdash/sprints/032-claude-feed-retirement.md`.

**3. The stem flip** — k-homelab `c78bfac`, `khlenv/store.yml`:
`KDASH_CLAUDE_REDIS` `rpidash2:6380` → `rpi53:6379`. **Live on the next resolve,
with nothing restarted** — which is CD-4's whole claim, observed rather than
assumed.

**4. The pin** — same commit, `manifests/common.yml`
`2.0.0-c3bbe7a` → `2.1.0-581799b`. `bin/apply` on kai and kubs0, `bin/audit`
clean on both afterwards (`claude-hooks: ok`, `kdeskdash-poll: ok`). cleo
refreshed by hand from the same store bundle and hash-verified —
`2bcfa823…`, matching the bundle's `SHA256SUMS`. All three publisher hosts then
report the same VERSION and the same sha256.

**5. Single-home proof, then the retirement.** A `SessionStart` fired through
the installed script on each of kai, kubs0 and cleo produced
`claude:session:{kai,kubs0,cleo}:flipprobe` on **central** and **nothing** on
`rpidash2:6380`. Both panels' configured claude path answered `PONG` against
`rpi53:6379` with `claude:limits` present and 3 live sessions visible; rpidash2
was in `claude` mode with an established socket to `192.168.1.213:6379`.

Then, by name from a captured snapshot rather than by pattern: **5 keys
deleted** — `claude:limits`, `claude:recent`, and three
`claude:session:*` — all of which had identical live counterparts on central, so
nothing unique was destroyed. Afterwards a **full unfiltered `--scan`** of
`rpidash2:6380` returns exactly four keys, all `kvscf:*`. CD-8 intact; the
instance retired a *feed*, not itself. A subsequent live write confirmed the old
home stays empty under traffic.

**6. klams** — the two records the overseer named
(comment 1212) superseded, plus the new end-state record:

| was | now | what changed |
|---|---|---|
| `01a016da-…` | `01a05e7b-4908-…` | "STILL UNVERIFIED: rpidash2/rpidash3's telemetry auth" → verified 2026-09-01, on-device |
| `019f9c03-…` | `01a05e7a-c68e-…` | rpidash3 reads the claude feed from central, not over the LAN from rpidash2 |
| — | `01a05e7b-b77d-…` | new: the end state, the rollback coupling, and the stem-before-pin ordering rule |

## Follow-ups

- The Python wrapper has no `hget` and does not need one. It gains a read when
  something it publishes needs a guard, on the same evidence-first footing as
  the rest of this repo's wrapper surface (CD-14).
