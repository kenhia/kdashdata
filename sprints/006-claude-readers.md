# 006 — The `claude:*` readers libkdash was missing

korg: proposal 1784, work items 1777–1782 and 1787. Slice 1 of program
korg:1785 (`claude:*` on the big panel). Overseen: an overseer session reviews
before and after the ship, and the ship waits on its green light.

## Goal

kstudiodash 005 went to build its claude panel and found the gap: the
`claude:*` schemas landed with the relocation close-out (sprint 005), but their
C projection never did. `kdash_feed.h` shipped typed readers for the five
schema'd kpidash feeds only, and `kdash_conn_t` deliberately exposes no raw
command path — so there was no way for a second dashboard to read the family
without hand-rolling a Redis client, which is the exact duplication this repo
exists to prevent.

Close it: key grammar, a parser shape for the registry's first HASH feeds, a
stem-parameterized connection handle, three readers, and the CMake knobs that
let a dashboard consume this repo at all.

## Premise check at the brief

Five of seven held as written. Two had drifted, both in ways that pointed the
same direction.

- **#1777, #1778, #1779, #1780 — hold.** `kdash_conn_opts_t` carried no stem
  and `kdash_conn.c:113` called `kdash_resolve_central()`, which hardcoded the
  central stem, the `KPIDASH_REDIS` alias and the `rpi53:6379` default.
  `kdash_keys.h` knew only the kpidash families. `kdash_payload.c` was entirely
  cJSON-over-a-buffer. `kdash_feed.c` spoke `GET`, `SCAN` and `SMEMBERS` and
  nothing else. kdeskdash's reference `claude_redis.c` measured 242 lines, as
  the item said.
- **#1781 — holds.** `kdash_ladder()` exists but is CD-6's 3-state ladder over
  a different shape; it is not the 5-state display status.
- **#1782 — drifted, same direction.** The item said CD-4 "currently describes
  one stem as if it were the only one". It does not: CD-4 has named
  `KDASH_CLAUDE_REDIS` since sprint 003. What it *did* still say was that the
  stem "is inert until the cutover slice points the publishers at it" — true
  when written, falsified by the cutover it was predicting. So the doc work
  became correcting that line rather than adding a stem to a list.
- **#1787 — drifted, and the mechanism changed.** The examples and tests were
  unconditional exactly as described. But `find_package(hiredis CONFIG)`
  returns `hiredis_DIR-NOTFOUND` on kai, so every build here takes the
  pkg-config path and the item's `hiredis::hiredis_static` target is
  unreachable on the machine kstudiodash will build on.

  The *reason* took a second look to get right, and the first answer was wrong.
  It is not that no config is installed: Ubuntu's `libhiredis-dev`
  1.2.0-6ubuntu3 ships `/usr/lib/x86_64-linux-gnu/cmake/**Hiredis**/HiredisConfig.cmake`
  — capital H, which `find_package(hiredis ...)` cannot match on a
  case-sensitive filesystem. Probed directly, `find_package(Hiredis CONFIG)`
  *does* succeed, and is still not something to switch to: it is a hand-rolled
  config that sets `HIREDIS_LIBRARIES` and friends, exports **no** imported
  targets at all, and its `HiredisConfigVersion.cmake` throws a CMake error
  when read. The aarch64 sysroot is the simpler case — `hiredis.pc`, no config
  of any kind.

  So the conclusion held and the knob stayed feasible; it just had to find
  `libhiredis.a` itself rather than ask a config for a target that does not
  exist on either path.

No cross-project plan applies — kdashdata is not in the routing table.

## Decisions

### Where the display ladder lives (#1781, now CD-16)

**Derivation and ordering in the library; formatting out.** Ken chose it, the
overseer concurred and added two requirements that made it a policy rather than
a split: the **thresholds** move in too (15 min idle, 40 min stale, and the
limits grace/legacy windows), as *parameters* with the constants as defaults —
the shape `kdash_ladder(age_s, idle_s, stale_s)` already had; and the 5-state
derivation **composes on `kdash_ladder()`** rather than sitting beside it, so
the next HASH-shaped feed reuses the ladder instead of writing a sixth state
machine.

One divergence from the overseer's literal wording, flagged rather than
silently taken. Read strictly, "fresh → the published status, idle → idle,
stale → stale" would drop kdeskdash's rule that `blocked` and `awaiting` stay
prominent through the idle band — a 20-minute-old blocked session would render
as IDLE. That rule is deliberate in the reference (it carries its own comment
there), so what shipped keeps it: the ladder owns the time bands, and inside
the idle band only `working` degrades. Age still wins at the stale boundary,
because the hooks cannot report a killed process. Filed as a comment on the
proposal so the overseer sees it before the green light rather than in a diff.

### The HASH parser shape (#1779, now CD-15)

The parser signature follows the shape of the data — a field/value list for the
two HASHes, a buffer for `claude:recent`, which really is JSON. Every rules.md
rule survives the change of shape; what is new is that the "is this a number"
question cJSON answered for free is now asked explicitly and strictly.

The field cap is **32**, not the reference's 16. That 16 sat exactly on
`claude:limits`' 16 documented fields, and both schemas are
`additionalProperties: true` — so a seventeenth field would have started
silently dropping a *required* one and rejecting valid records. Cheap to widen
now, invisible to debug later.

### One walk, parameterized by stem (#1777)

`kdash_stem_t { key, legacy, fallback }` carries the CD-4 walk's two optional
steps as data, and `kdash_resolve_endpoint(&stem, ...)` runs it.
`kdash_resolve_central()` is that call with `KDASH_STEM_CENTRAL`, kept by name
because every kpidash reader already uses it. This is the C spelling of the
`Stem` the Rust and Python wrappers have carried since sprint 003 — the
existing shape was adopted rather than a third invented.

The claude stem has **no fallback**, and that asymmetry now reaches readers:
resolving it with khlenv unreachable returns the new `KDASH_EP_UNRESOLVED`
rather than a guess. A panel rendering confident rows out of the Redis nobody
is writing to is worse than one rendering "unavailable".

### Two smaller calls, made deliberately

- **An absent `project` stays `""`.** The reference substitutes `"?"`; that is
  a placeholder, i.e. rendering, and CD-10 keeps it on the panel's side.
- **`cwd` is parsed and discarded.** `project` is already its basename and no
  dashboard renders the path, so carrying it would cost 256 bytes a row for
  nothing. Documented in the header so the next reader knows it was a choice.

## What shipped

**Pure core** (host-tested, no Redis):

- `kdash_keys.h` — `claude:session:<host>:<sid>` parse and build, plus the two
  literals. Its own entry point rather than bending
  `kdash_client_key_parse`'s mould: four segments whose last is an opaque
  session id has no fixed suffix to anchor on. `KDASH_CLAUDE_KEY_MAX` is
  deliberately separate from `KDASH_KEY_MAX`, which sizes the kpidash readers'
  SCAN buffers and would have grown their stack frames for a family they never
  touch.
- `kdash_payload.h/.c` — the three record types, the two field/value parsers
  and the cJSON one, the status/disp enums with their `_str` helpers, the
  derivation, the attention-first sort, and per-gauge limits staleness.
- `kdash_freshness.h` — the family's four time constants, beside the windows
  the other families already keep there.

**I/O shell** (verified live, not by `just check` — CD-10):

- `kdash_endpoint.h/.c` — `kdash_stem_t`, the two stem constants,
  `kdash_resolve_endpoint()`, `KDASH_EP_UNRESOLVED`.
- `kdash_conn.h` — `opts.stem`, copied by value onto the handle so a
  stack-allocated stem is safe.
- `kdash_feed.h/.c` — `kdash_claude_sessions()` (SCAN + HGETALL),
  `kdash_claude_limits()` (HGETALL), `kdash_claude_recent()` (LRANGE).
  Commands ported from kdeskdash; **pacing not** — its resumable step machine
  exists because an LVGL timer drives it there, and these are one-shot under
  `KDASH_SCAN_BATCHES`. Session discovery keeps no raw-key buffer at all: each
  key is parsed at the choke point as it arrives and only its two validated
  segments are kept, straight into the caller's array.

**Consumability** (#1787): `KDASH_BUILD_EXAMPLES` and `KDASH_BUILD_TESTS`,
defaulting to whether this is the top-level project, and `KDASH_HIREDIS_STATIC`
which prefers `hiredis::hiredis_static` when a CMake config offers it and
otherwise finds `libhiredis.a` itself — the path that actually fires here, on
both architectures.

`CMakeLists.txt`'s own hiredis comment claimed the CMake config was preferred
"on the build host". Measured, that branch has never fired on either build
path, so the comment now records what was actually found — including that
`find_package(Hiredis)` is a trap rather than the fix, so the next reader does
not "correct" the case and get a config with no targets and a version file that
errors.

**Docs**: CD-15, CD-16, CD-4 corrected and extended, the registry's claude
section pointed at the readers, README, roadmap.

## Negative tests

Every gate was seen to fail before it was believed.

| Planted fault | Caught by |
|---|---|
| Drop the sid from the key grammar's validation | 10 checks, including the fifth-segment and empty-sid cases |
| Unknown `status` defaults to `working` instead of rejecting | 2 checks — the resurrection-race guard |
| `blocked`/`awaiting` degrade to idle (the literal ladder composition) | 2 checks |
| The claude stem resolves through the central one (the sprint's trap) | 6 checks |
| `KDASH_HIREDIS_STATIC=ON` with no archive findable | CMake warning, falls back to shared and says so |

One test was found to be proving the wrong thing and was tightened: removing
the "a stampless scoped set is always stale" guard did **not** fail, because
`age since stamp 0` is astronomically large at any real `now`. It now also
asserts the rule at a `now` small enough that the arithmetic alone would call
the gauge fresh — and with that, removing the guard fails.

Legacy-alias-after-unreachable-khlenv has no test and knowingly so: asking a
down service twice is a latency bug, not a wrong answer, and no unit test
without a fake khlenv can see the difference.

## Live verification

`just dump` against `rpi53:6379` with `REDISCLI_AUTH` from the CD-12 env file.
Both stems resolved and connected **separately**, and all three claude feeds
read:

```
KDASH_CENTRAL_REDIS  rpi53:6379 (connected)
KDASH_CLAUDE_REDIS   rpi53:6379 (connected)
...
== claude sessions (claude:session:*) ==
  awaiting  cleo         ClaudeWorks      21s ago  Fable 5.1  "SSH-driven overseen sprints with MCP"
  awaiting  cleo         ClaudeWorks      714s ago  Fable 5.1  "Claude mode via kdashdata"
  awaiting  kai          kstudiodash      1666s ago  Opus 5  "Start sprint korg:1728"
  working   kai          kdashdata        69s ago  Opus 5  "Overseen sprint korg:1784"
  4 session(s), 0 skipped

== claude usage limits (claude:limits) ==
  five-hour   4.0%   seven-day   2.0%   (observed 258s ago)
  scoped Fable      2.0%     (observed 258s ago)

== claude recent (claude:recent) ==
  8 entry(s), 0 skipped
```

The sprint's own session appears as `working` on kai/kdashdata, and the
ordering is the derived one — awaiting above working, most recent first within
a rank. Zero skips across all three feeds.

**Consumer shape** (#1787's acceptance criterion), a throwaway parent project
that `add_subdirectory`s this repo and links `kdash`:

- embedded build produces `panel` **only** — no `kdash_dump`, no `test_*`, and
  `ctest` in the parent reports "No tests were found";
- with `-DKDASH_HIREDIS_STATIC=ON`, `ldd` shows no `libhiredis.so` dependency
  and the binary still reads the live feed. That is kstudiodash's "build on
  kai, scp one binary, install nothing" story, proven rather than assumed.

Both architectures build clean with `-Wall -Wextra`: x86_64 natively, aarch64
via `just build-aarch64` against the Pi sysroot. `KDASH_HIREDIS_STATIC=ON`
works on the cross build too, finding
`~/pi5-sysroot/usr/lib/aarch64-linux-gnu/libhiredis.a` — better than #1787
predicted, since `find_library` needs no static/shared split in the config to
locate an archive.

One thing about `kdash_dump`'s output worth knowing, found by the overseer
re-running it: **without `REDISCLI_AUTH` in the environment both stems report
`(unreachable)`**, which is indistinguishable from the host being down. That is
CD-2 and CD-6 behaving exactly as designed — the handle's PING fails, and a
degraded reader does not get to explain itself — but "unreachable" in this tool
can mean "unauthenticated". The dump above was run with the CD-12 env file
sourced.

## Follow-ups

- **kdeskdash adopts these readers** (kdeskdash korg:1783). Until it does, the
  claude logic exists twice — accepted here and not scheduled here, because
  migrating a panel people look at daily is a different risk from adding a
  reader nothing consumed yet.
- **korg:1790 (XS, backlog, not covered here).** `kdash_claude_sessions()`
  returning early on a mid-scan `KDASH_UNAVAIL` hands back a partial count
  indistinguishable from a complete one, and skips writing `*skipped`.
  `kdash_conn_reachable()` is already false at that point so a panel has its
  signal, but the header does not say so. Filed by the overseer; it inherits
  the behaviour of the kpidash SCAN readers, and changing one means changing
  all three.
