# 012 — The agent-activity feed contract, and a ruling on GHCP sessions

korg: proposal 2747, work items 2741, 2742. Slice 3 of the kxeneon Agents panel
program korg:2751, run as an overseen sprint in karc leg `kdashdata-111d6a` on
kai. Overseen: an overseer session reviews before and after the ship, and the
ship waits on its green light.

## Goal

Two contract decisions that everything behind them is blocked on, and no code.
kxeneon's Agents panel wants a klaude-top verdict per session row, and it
already reads a `ghcp:session:*` namespace that nothing on the fleet writes.
Neither the klaude-top publisher slice (korg:2748) nor the Copilot publisher
slice (korg:2755) can start until the shapes exist here, because in this repo a
feed exists when its schema lands — a publisher writing a key with no schema in
kdashdata is off-contract by rules.md.

So: land `kdash:agentact:{host}:{sid}` and `ghcp:session:{host}:{sid}`, their
registry entries, and the decisions that justify the two places each breaks a
house default.

## Premise check at the brief

Both hold. One drifted in a way that changed a deliverable, and one turned out
to be checkable only by going and looking, which is most of what this sprint
did.

- **#2741 — holds, field list drifted wider.** klaude-top's `--json` Snapshot
  (`internal/monitor/monitor.go`) carries everything the item named, plus
  `pid`, `name`, `state`, `disk`, `threads`, `sockets` and `transcript`. Its
  `ts` is `now.UnixNano()/1e9` — already unix seconds float, already
  `snake_case`, so the payload is the struct rather than a translation of it.

  All of those are schemaed. **`name` is not incidental**, which the first cut
  of this schema got wrong by leaving it to `additionalProperties`: klaude-top
  derives it from the `-n <name>` karc gave the session, so an overseen leg
  publishes as `overseen 2747` and the row Ken is watching is identifiable as
  the leg the overseer launched — which is this feed's stated purpose, not a
  nicety. Caught in overseer review and fixed before the ship; `transcript`
  was documented in the same pass as the host-local diagnostic it is.

  The drift that mattered: the item floated optional `in_hist` / `cpu_hist`
  sparkline arrays as a payload addition, and klaude-top marks exactly those
  two slices `json:"-"` with the comment *"for the terminal UI only — never in
  --json"*. They are not available to a publisher today. Resolved by schemaing
  them as an optional capped slot rather than by asking klaude-top to change:
  the contract opens the door, the panel slice decides whether to walk through
  it, and neither is blocked on the other.

- **#2741's join premise — verified live, not assumed.** The item asserts that
  `{sid}` can be the transcript's uuid filename and will match the hooks'
  session id. It does: `claude-pub.sh` keys on the hook payload's `session_id`,
  and this leg's own `$CLAUDE_CODE_SESSION_ID` is exactly the basename of its
  transcript file on kai. Checked because CD-20 is built on it and the failure
  mode if it were false is a join that silently returns nothing (see below).

- **#2742 — holds; its stdin claim was about the wrong file.** The item says
  `session.start` carries `sessionId`, `cwd`, `gitRoot` and `copilotVersion`.
  That is `session-state/<id>/events.jsonl` — the session's own log. The
  **hook's stdin** carries `sessionId`, `timestamp`, `cwd` and per-event
  extras, and nothing else. `gitRoot`, `copilotVersion`, and any model or title
  never reach a hook. This is the difference between "the data exists
  somewhere" and "the publisher is handed it", and it is why `model` and
  `title` are documented as normally absent rather than merely optional.

- **#2742's reader premise — not checkable from here, and said so.** "kxeneon
  reads `ghcp:session:*`" was recorded by a session on cleo; kxeneon lives at
  `D:\ClaudeWorks\kxeneon` and is in no kaed root reachable from kai. Taken on
  the planning session's record rather than re-verified, and flagged at the
  brief rather than quietly assumed.

No cross-project plan applies — kdashdata is not in the
`cross-project-planning` routing table.

## The Copilot hook probe

WI 2742 asked the sprint to confirm Copilot CLI's event set and "exactly what
arrives on the hook's stdin" before choosing. There is no documentation on the
box: `copilot` on kai is a 160 MB compiled Node SEA, and every candidate event
name is absent from its strings, so the JS is compressed inside the blob. The
only ground truth available is a live one.

Method: a temporary `~/.copilot/hooks/kdashdata-probe.json` declaring all six
candidate events plus six speculative ones, each appending its name and its
stdin to a file in `.scratch/`; then two real `copilot -p` sessions on kai, one
of which ran a tool. The probe file was removed afterwards and the pre-existing
unmanaged `klams-sync.json` was left untouched (verified by mtime — still
2026-06-08).

**Result — the complete user-level hook set on Copilot CLI 1.0.83:**
`sessionStart`, `sessionEnd`, `userPromptSubmitted`, `preToolUse`,
`postToolUse`, `errorOccurred`. `errorOccurred` did not fire (nothing errored)
and is recorded as documented-but-unobserved rather than confirmed. Every
payload carries `sessionId`, `timestamp`, `cwd`; `sessionStart` adds `source`
and `initialPrompt`, the tool events add `toolName`/`toolArgs` and
`postToolUse` a `toolResult`, `sessionEnd` adds `reason` (`complete`).

Three findings shaped the contract, and none was guessable from a field list:

1. **There is no turn-end event.** Nothing fires when the agent finishes
   replying. A publisher can raise `working` and DEL on `sessionEnd`; it can
   never observe `awaiting`. So a Copilot session genuinely waiting on its user
   keeps saying `working` until the reader's freshness ladder ages it — a known
   blind spot that belongs in the schema, where a reader meets it, not in the
   publisher that could not have avoided it.
2. **`timestamp` is milliseconds**, and `ts` is unix seconds everywhere here. A
   pass-through puts every record a thousand-fold into the future; readers
   treat negative ages as skew and therefore fresh, so a session that ended
   weeks ago would read as live forever with every publisher succeeding and
   every reader parsing.
3. **`userPromptSubmitted` fires before `sessionStart`**, reproducibly, by
   ~4 ms, in `-p` mode. A publisher treating `sessionStart` as the first write
   clobbers a record that already exists.

Two incidental facts worth the recipe slice's attention (korg:2756): the six
speculative event names fired nothing and drew **no warning**, so a mistyped
event name in a hook file fails silently; and the log records "Loading repo
hooks in prompt mode", so repo-level hooks exist alongside the user-level ones
the recipe installs.

## What shipped

**`contracts/schemas/kdash-agentact.schema.json`** — latest-value expiring,
`SET … EX ~10`, exactly 4 segments, required `verdict` + `ts`, everything else
optional so a tick that lost a counter still publishes liveness. Verdicts are
klaude-top's own uppercase `WAITING`/`WORKING`/`TOOL`/`STALLED`, carried
verbatim rather than case-folded — a transform is a second place for the two
ends to disagree. `transcript_age` is explicitly nullable (`null` means looked
and found nothing, which is not the same as absent, and coercing it to 0
reports a hung session as having just written); `transcript_growth` has no
minimum because a rotated transcript makes it negative.

**`contracts/schemas/ghcp-session.schema.json`** — HASH, TTL 7200 s,
`claude:session`'s field set name-for-name including an enum wider than Copilot
can fill, with the three measured findings above written into the fields they
constrain.

**`contracts/registry.md`** — the `agentact` row in the `kdash` family, a new
`Family: ghcp` section, `ghcp:*` added to the Claude feed's home, and the
Reserved list updated to name the exception rather than leave it to be
inferred.

**`contracts/rules.md`** — `ghcp:*` recorded as one named namespace exception,
with its consequence (new, so frozen from day one; a change is a versioning
event, not a migration) and an explicit "this is not a precedent".

**`docs/architecture.md` CD-20** — two families joining on a shared key, and
the rule that the shared identity must be verified against running data before
it is relied on. The failure it avoids is the one this repo has no other
instance of: two families keyed on tokens that merely resemble each other
produce a join returning nothing, correctly, at every level — both publishers
work, both readers parse, every key is well-formed, nothing is malformed to
skip, and the panel just never shows the halves together. It looks like a
feature nobody finished.

**`docs/architecture.md` CD-21** — a new family outside `kdash:` with its
reason, and the deliberate choice that mirroring `claude:session`'s field set
beats describing Copilot accurately, because consumer reuse is worth more than
a better name. Plus the general form of the probe's lesson: a hook contract's
guarantees are its event ordering and its units, and neither is visible in a
field list.

## Decisions

- **New expiring family, not additive fields on `claude:session`** (as WI 2741
  recommended). The hooks own that hash and `sessionEnd` DELs it; a second
  writer HSETting afterwards resurrects the key with no TTL, and the
  resurrection-race guard protects readers, not the keyspace.
- **The sparkline arrays are a slot, not a field.** Schemaed optional and
  capped at 24; klaude-top does not emit them and is not asked to.
- **Key absence on `agentact` means *unknown*, never idle.** klaude-top is
  Linux-and-`/proc` only, so cleo will never write this feed. That is a fact
  for the registry, not a gap to close.
- **`ghcp:` keeps its name**, as a recorded exception rather than a slip.
- **No `ghcp:limits` and no `ghcp:recent` yet.** The sprint-004 ruling on
  Microsoft-managed quota stands; `recent` is one extra `LPUSH` on the day
  somebody asks, and `sessionEnd`'s `reason` is already there for it.
- **No C reader in this sprint.** kxeneon reads Redis directly and kdeskdash
  has not asked. Both feeds are registered ahead of their consumers, as `stale`
  was.

## Verification

`just check` — all four gates green: `check-docs` (every JSON parses, every
relative link resolves, every schema is listed in the registry, `.ps1` pure
ASCII, CLAUDE.md's Status line current), `check-python`, `check-rust`, and the
CMake build plus ctest.

Both schemas were then validated against **real** payloads rather than invented
ones — a one-off script in `.scratch/`, deliberately not wired into `just
check`, because `jsonschema` is not a dependency of this repo and adding one to
make a gate is not a trade this repo makes:

- `agentact`: four rows of live `klaude-top --json --once` output from kai,
  built from source for the purpose. One of the four is this leg itself
  (`"name": "overseen 2747"`), whose `transcript` basename is this session's own
  id — CD-20's join demonstrated on running data rather than argued.
- `ghcp:session`: the six captured hook payloads, projected through the
  transforms the publisher will apply (millisecond divide, `cwd` basename).

**The negative tests found a real gap, and it was the important one.** Ten
planted errors were checked for rejection. Nine were rejected. The tenth — a
millisecond `ts` on `ghcp:session`, this feed's single sharpest documented
hazard — sailed through: `"type": "integer"` accepts `1789620973658` as
happily as `1789620973`. The schema described the trap at length in its own
`description` and did nothing to stop it.

Fixed by pinning it structurally: `ts` and `started_ts` are bounded
`exclusiveMinimum: 0, maximum: 1e11`. 1e11 seconds is the year 5138, and every
millisecond stamp after 1973 exceeds it, so a unit slip now fails validation
instead of publishing a record that reads as permanently fresh. The same bound
went on `agentact`'s `ts`, where it guards a future writer rather than the
current one. This is CD-18's `stale: const true` argument reused: **when the
whole hazard is that a wrong value looks plausible, the schema has to refuse
it, because prose in a `description` is read by people and not by validators.**

The two accepted-cases were checked too, since a schema that over-rejects is
its own bug: `transcript_age: null` and a negative `transcript_growth` both
validate.

No live Redis verification — this sprint publishes nothing. The feeds are
contract only; their first live keys arrive with korg:2748 and korg:2755.

## Follow-ups

One new item, filed because it names a decision this leg could not make:

- **WI 2761** — should `claude-session.schema.json`'s `ts` carry the same bound
  `ghcp-session`'s now does? The decision it turns on is whether tightening a
  *grandfathered* schema counts as a breaking change under rules.md's
  versioning rule when no current writer violates it. Both readings have force
  — the bound rejects only already-wrong records and `claude-pub.sh` has always
  written seconds; but CD-3's posture is that grandfathered shapes are
  documented as-is and never tightened underneath a live publisher this repo
  does not own. Not a repair, because the evidence did not remove the choice.
  Until it is answered, CD-21's "field-for-field identical" has exactly one
  deliberate exception, and both the schema and the registry say so.

**Repaired in passing:** none. The one defect this sprint found — the schema
accepting a millisecond `ts` — was in its own new work, caught by its own
negative test, and fixed before it landed.

Two more belong to slices already in the program and are not filed as new
items:

- The publisher that fills `kdash:agentact` is klaude-top's `--publish`
  (WI 2743, slice korg:2748), which must derive `{sid}` from the transcript
  filename per CD-20.
- The publisher that fills `ghcp:session` is kdeskdash's `publisher/ghcp-pub.sh`
  (WI 2753, slice korg:2755), which must divide `timestamp` by 1000, take
  `started_ts` from the payload rather than from write order, and never emit
  `awaiting` or `blocked`.
