# Sprint 016 — A fault-injection seam for the counted readers, and a verb that proves auth

**Proposal:** korg:3046 (slice 2 of the low-hanging-fruit run 2 program,
korg:3062)
**Covers:** WI 2246, WI 2492
**Branch:** `016-fault-injection-seam-and-auth-verb`

## Goal

Two things this repo had been asserting rather than checking.

1. **WI 2246** — the counted readers publish a contract (`-1` means the read
   did not complete; `out` zeroed, `*skipped` 0) that nothing could exercise,
   because triggering it means losing the endpoint mid-SCAN on a Redis three
   dashboards read.
2. **WI 2492** — `kdash-pub endpoint` is what the repo points at for live
   verification, and it issues no command, so `--no-auth` exits 0 against a
   Redis that requires a password.

Both options were already written out with their costs; the overseer had made
the calls (proposal notes). Neither needed Ken.

## Premise check

Run before any work, at 2026-09-21 21:38 PDT on kai.

- **WI 2246 — premise holds.** `kdash_feed.c` builds only into the `kdash`
  target; of the four test binaries, three link `kdash_core` and the fourth
  (`test_endpoint`) makes no connection. Nothing under `tests/` names
  `kdash_services`, `kdash_apttemps` or `kdash_claude_sessions` — the only
  caller is `examples/kdash_dump.c`, which is the live toy consumer.
- **WI 2492 — premise holds, and re-measured.** The item's table was measured
  on kubs0 on 2026-09-12; reproduced on **kai**, 2026-09-21, against the same
  authenticated `rpi53:6379`:

  | invocation | exit |
  |---|---|
  | password from the per-host file | 0 |
  | `REDISCLI_AUTH=definitely-not-the-password` | 2 — `Password authentication failed` |
  | `--no-auth` | **0** |

  Nine days on, unchanged.

No cross-project plan applies: kdashdata is not in
`cross-project-planning/index.md`.

## Decisions

### CD-10 amended — the I/O shell gets one seam, for one contract

Recorded in full in `docs/architecture.md`. The short version: CD-10 keeps the
I/O shell out of `just check` and that was right, except for the one rule in
the shell with real consequences. `src/kdash_feed.c`'s Redis calls now go
through four function pointers (`src/kdash_feed_internal.h`) — `scan_keys`,
`get_string`, `get_hash`, `free_reply` — and `tests/test_feed.c` swaps them for
a fake that answers from a table and fails the Nth read on demand.

The boundary is what keeps this a narrowing and not an exception:
`kdash_conn.c` is untouched and still verified live; nothing is in
`include/kdash/`, so a dashboard links the same API it linked before; the real
implementation is installed at load with no initialisation call, so a consumer
that never hears of the seam gets hiredis; and it is four pointers and one
fake, not a mock framework.

`free_reply` is the member worth justifying, since the overseer's brief named
only two. Without it the fake would have to allocate its replies with whatever
allocator `freeReplyObject` happens to free with — true today by coincidence,
and nothing enforces it. With it, "a reply the seam produced is freed by the
seam" is a local rule the compiler checks. `scan_keys` is there because a
*mid-list* drop needs a list, and the list comes from a SCAN.

`kdash_clients()` (SMEMBERS) and `kdash_claude_recent()` (LRANGE) deliberately
do **not** go through the seam: they are single round trips, the mid-list rule
is vacuous for them, and there is nothing there the seam would make testable.

The rejected alternative was the fake RESP server over a unix socket — it buys
the whole class rather than this one contract, and costs an M-sized test server
that is itself untested.

### CD-25 — `endpoint` answers *where*, `check` answers *whether*

Option 2 from WI 2492, as the overseer called it: a new verb, and `endpoint`
untouched. Changing what exit 0 means for every existing caller of a published
CLI is a contract change; adding a verb is not.

`kdash-pub check` does everything `endpoint` does and then round-trips a
`PING`. Exit **0** accepted / **1** khlenv holds an explicit null for this
stem (deliberately nowhere — an answer, not a fault) / **2** could not ask.
That is `get`'s three-outcome shape, which this binary already documents, and
`--best-effort` leaves all three alone for the reason it leaves `get`'s alone:
on a verb whose exit status *is* the answer, folding "I could not ask" into 0
is how a probe starts lying.

What exit 0 proves is stated narrowly: resolved, connected, and the server
accepted an **authenticated** command. Not that this connection may *write* —
that would need a write, and a probe that writes is not a probe.

**Does the Python wrapper's `Publisher.connect()` need the same verb? No.**
It has the same property and worse — redis-py builds a pool and opens no
socket at all — but it was never sold as a probe. Every path that reaches
Redis (`get`, `scan`, `publish_latest`, `publish_expiring`, `publish_event`)
issues a real command and authenticates for real. There is no false claim to
fix, and adding a probe would be inventing one. What *was* overstated is the
docstring's "a live `redis.Redis`"; that is corrected to say it opens no socket
and proves nothing.

The sharp edge on `endpoint` is now documented where somebody meets it: in
`--help`, in the binary's own module docs, in `publishers/README.md`, in the
root `README.md`, in the `justfile` next to both recipes, and in CD-25.

## What shipped

**WI 2246**

- `src/kdash_feed_internal.h` — the seam, with the rationale and the boundary.
- `src/kdash_feed.c` — the three real helpers renamed `real_*`, the table, and
  the eight reader call sites routed through it.
- `tests/test_feed.c` (new, registered in `CMakeLists.txt`) — 30 checks over
  all three counted readers plus the single-key path.
- `include/kdash/kdash_feed.h` — the counted-reader block says the rule is now
  under test and where.

**WI 2492**

- `publishers/rust/src/lib.rs` — `Connection::check()`, and
  `Error::UnexpectedReply` so "answered something else" is not folded into
  "did not answer".
- `publishers/rust/src/bin/kdash-pub.rs` — the `check` action, `EXIT_NOWHERE`,
  the exit-code table, the help text, and four new unit tests.
- `justfile` — `pub-check`, next to `pub-endpoint`, each saying which question
  it answers.

## Repaired in passing

1. **The `-1` contract was only half-kept on the SCAN-failure path.** All
   three counted readers returned `-1` from a failed SCAN without zeroing
   `out`, which `kdash_feed.h` already promised they would not. Worst in
   `kdash_claude_sessions()`, whose visitor parses each key straight into `out`
   as the SCAN yields it — so a SCAN failing after an earlier batch left rows
   carrying a host and a sid and no payload at all. Found by writing the tests,
   on their first run; fixed in `src/kdash_feed.c` and covered by three checks
   in `tests/test_feed.c`.

2. **`.github/copilot-instructions.md` had been stale since sprint 001.** Its
   `## Project` section still read "Status: contract v0 landed (sprint 001)…
   Next: the shared C consumer library (sprint 002)" and "decisions CD-1…CD-6"
   — a "Next" that shipped fourteen sprints ago. The two files' harness halves
   are byte-identical, so the Project section is plainly meant to be a mirror;
   it just had nothing looking at it. Synced from `CLAUDE.md`.

3. **...and `scripts/check.py` now gates both orientation files, not one.**
   The rule already existed — "CLAUDE.md's Status line names the newest sprint
   record", WI 1928 — and applying it to the file next to it is that same rule,
   not a new one. Without this, (2) is a fix with a fourteen-sprint half-life.
   Negative-tested.

4. **Three doc lines that overclaimed `endpoint`.** `publishers/README.md`
   called `kdash-pub --app kdashdata endpoint` "the stronger per-host check:
   it proves khlenv resolution and the CD-12 auth route work on that host";
   the root `README.md` and `CLAUDE.md` said the same in shorter form. It
   proves the first and not the second, which is the whole of WI 2492. All
   three now point at `pub-check`.

## Gates

`just check` is green: `check-docs`, `check-python`, `check-rust`, and the
CMake build with **five** ctest suites (`test_feed` is new).

**Negative-tested, both new gates.**

- `test_feed`: planted the pre-sprint-009 bug (hand back the rows gathered
  before the drop) in `kdash_services()` **and** reverted repair (1) in
  `kdash_apttemps()`, rebuilt, and watched five checks fail on exactly the
  right lines — the partial list, the un-zeroed buffer, the drop-on-first-read,
  and the un-zeroed SCAN failure. Restored; green again.
- the orientation gate: set the copilot file's Status paragraph back to its
  stale sprint-001 text and watched `check-docs` name it and exit 1.

## Live verification

Everything below ran **on kai**, the host that publishes, against the real
`rpi53:6379`. None of it is a soak: WI 2492's acceptance is a probe, and a
probe fires on demand.

| invocation | `endpoint` | `check` |
|---|---|---|
| password from the per-host secrets file | 0 | 0 |
| `--no-auth` | **0** | **2** — `NOAUTH: Authentication required` |
| `REDISCLI_AUTH=<wrong>` | 2 | 2 |
| `--endpoint 127.0.0.1:6399` (nothing listening) | — | 2 — `Connection refused` |
| `--stem KDASH_CLAUDE_REDIS` | — | 0 |
| `--stem KDASH_NO_SUCH_STEM` | — | 2 — khlenv holds no value at any level |

The hole WI 2492 measured is closed: the row that used to read 0 for a
configuration that cannot write a single key now reads 2, and says why.

`check`'s **exit 1** (khlenv holds an explicit null) is the one branch not
fired live, because no stem in the fleet is an explicit null today. It shares
`endpoint`'s `Resolved::Nowhere` branch, which has printed `(none)` since
sprint 003, and the resolver's own Nowhere case is unit-tested in
`publishers/rust/src/endpoint.rs`. Stated rather than glossed.

## Follow-ups

None filed. Everything this sprint turned up was settled by evidence already in
front of it and is in "Repaired in passing" above.

Out of scope by the proposal's own sequencing: knarr's allowed-absent host
(korg:3052) is what lets `just deploy-komarchy` and `deploy-all`'s probe fold
back into `deploy`. Not touched here.
