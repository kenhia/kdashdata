# Sprint 022 — Registry: remove the retired kpidash-internal keys

**Proposal:** korg:4598 (slice 3 of program korg:4599, "Retire kpidash's
unread producers")
**Covers:** no work item, on purpose — the proposal's notes are the spec
**Branch:** `022-registry-retire-kpidash-internal-keys`
**Run as:** karc leg `kdashdata-3095c3` on kai, unattended

## Goal

kpidash sprint 032 (korg:4562) dropped the dashboard's activities, repo-status,
fortune and dev-graph widgets; sprint 036 (korg:4596) shipped kpidash-client
2.0.0 without their producers and deleted the keys on central. The registry's
"kpidash-internal" table still listed them, and the Python publisher README
used `kpidash:activities` as its `publish_event` example. Docs only.

## Premise check

Run on kai, 2026-11-13, against kpidash `origin/main` at `70b7f7a`.

- **Every row to remove has neither reader nor writer.** `git grep` of
  kpidash's `src/`, `clients/` and `include/` finds none of
  `kpidash:activit*`, `kpidash:repos`, `kpidash:fortune`, `kpidash:cmd:graph`
  or `fortune_dev`; the only hits are docs recording the retirement
  (`docs/CLIENT-PROTOCOL.md` §4–6 and §9.3/9.4, `docs/ARCHITECTURE.md`'s
  removed-widgets and removed-producers tables, the client README).
- **Nothing else in the fleet touches them.** A kaed fan-out search of
  `*:src` (kai, kubs0, kubsdb) found matches only in kpidash's docs and
  sprint history, and in the vendored `lib/kdashdata` copies of this repo
  inside kpidash and kdeskdash — which are this registry, and update on their
  next real submodule bump. kubs0 and kubsdb: zero matches, untruncated.
- **The rows that stay still have readers.** `kpidash:cmd:grid`, `textsize`,
  `services:evict`, `kpidash:screenshot` and `kpidash:status:ack:*` are all
  defined in kpidash's `src/protocol.h` on main.
- No guiding plan lists kdashdata (`cross-project-planning/index.md`).

## Decisions

- **A retirement note under the table, not a silent deletion.** The repo
  keeps no changelog or retired-keys file; its precedent is an inline note
  where the rows were (`kdash:panel:kstudio`, sprint 020). The note names the
  keys, both kpidash sprints, and kpidash's `last-with-dev-graphs-fortune` tag
  where the shapes survive.
- **README example → `claude:recent`.** It is a live capped event log that
  `publish_event` actually matches (LPUSH + LTRIM, cap 20). The old example
  was wrong twice over: `kpidash:activities` was a ZSET + HASH log, which
  `publish_event` never wrote. The new payload carries the schema's required
  fields bar `ts`, which the wrapper stamps.
- **No kdashdata submodule bump in kpidash or kdeskdash** (proposal notes):
  this rides along with their next real bump.

## What shipped

- `contracts/registry.md`: three rows gone from "kpidash-internal"; `graph`
  and `fortune_dev` gone from the `kpidash:cmd:*` row; a retirement note.
- `publishers/python/README.md`: the `publish_event` example.
- Status lines in `CLAUDE.md` and `.github/copilot-instructions.md`.

## Evidence

`just check` green on the branch (exit 0, all four gates); merged as `617ff8e` (PR #23).

## Repaired in passing

- The Python README's `publish_event` example named a key of the wrong type
  for the call it demonstrated; fixed with the same edit.

## Follow-ups

None.

## Deployed

2026-11-13, from merged `main` at `617ff8e`, run from kai as karc leg
`kdashdata-3095c3`. These are the `.sprint-deploy` steps:

- `just publish` printed "nothing to publish: 0.1.0-4837473 already in the
  store". That is a deliberate no-op, because this sprint changed no publisher
  input.
- `just deploy` installed `kdash-pub 0.1.0-4837473` (unchanged) on kai,
  kubs0, cleo, kimac and thrall. knarr skipped three hosts it is allowed to
  skip, and exited 0:
  - komarchy: ssh timed out.
  - kbook2: ssh timed out.
  - thrall-arch: its hostname did not resolve.

  These skips are what knarr reported from kai. They are not a reachability
  verdict from anywhere else.

Nothing in this sprint has a live surface to smoke-test: the registry and the
README are documentation.
