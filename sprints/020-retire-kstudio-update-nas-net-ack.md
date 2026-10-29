# Sprint 020 — Retire the kstudio namespace; register the update, NAS-health and net-ack contracts

**Proposal:** korg:4152 (a slice of low-hanging-fruit program korg:4162, run 5)
**Covers:** WI 3986, WI 3641, WI 3955, plus the CD-27 addendum kpidash WI 4143 needs
**Branch:** `020-retire-kstudio-update-nas-net-ack`
**Run as:** karc leg `kdashdata-a01c09` on kai, overseen

## Goal

Four contract-side changes, none of which needs a running writer:

- kstudio left the homelab on 2026-10-20, so its namespace leaves the
  contracts (3986).
- The weekly apt run gets a proper update-issue feed to move to off its
  interim `kdash:stale:*:apt-update` route (3641).
- gratch's big panel gets a NAS-health contract (3955).
- kpidash's network card gets an ack key, so a reviewed warning can be
  cleared (4143's contract half).

The overseer made the main calls in the proposal notes, and this sprint takes
them. Where one turned out to be wrong or a no-op, that is recorded below.

## Premise check

Run from kai, 19:02 PDT 2026-10-28.

- **3986 holds.** Every location the WI lists still names kstudio. A fresh
  grep matched the WI's list and found three more: the example app names in
  `README.md`, `include/kdash/kdash.h` and `include/kdash/kdash_feed.h`.
- **3641 holds.** No `update` family existed. k-homelab's interim route is
  live: `kdash:stale:{gratch,kai,kubs0,kubsdb}:apt-update` were all on central
  when SCANned.
- **3955 drifted, in the direction the brief predicted.** gratch is Ubuntu +
  ZFS. I read `/run/gratch-ui/privileged.json` on gratch (ssh as ken from kai).
  It holds one raidz1 pool `nas` (three 4 TB NVMe), four ATA disks outside
  the pool (an 860 EVO and three IronWolfs, in standby), plus a 990 PRO.
  It does not hold `/mnt/disk1`, `/mnt/cache` or root-filesystem usage. The
  contract is designed from the real file.
- **4143's ack** — the decision the WI named (panel or producer) is the
  overseer's call: on the panel. Checked that v1 alone is enough for the
  panel to apply it (below).

No cross-project plan applies: kdashdata is not in the planning index.

## Decisions

- **The registry's dashboard-local table grew a Status column**, with the
  family vocabulary (`live` / `migrating` / `retired`). The notes said to mark
  the namespace retired rather than delete it, and the gate excluded retired
  *families* but read every row of the local table. Once `kstudiodash` left
  the allowlists, the gate said so at once ("registry.md documents the
  `kstudiodash` namespace and NAMESPACES does not list it"). Now `check-docs`
  reads the column. A status outside the vocabulary is an error, so the gate
  cannot go blind if the column is dropped. `kdeskdash:*` is `migrating`,
  which is what its row already said in prose (program korg:2935).
- **The `kdash:panel` family stays; only its kstudio instance is retired.**
  It now has no reader. Retiring it would mean removing `kdash_panel()` from
  the API in two vendoring repos, which is a decision, not housekeeping. The
  schema and reader keep working as a channel for any future dashboard host.
  The registry says the family has no reader today.
- **`kdash:update:{host}:{kind}`**, one key per kind, as the notes ruled.
  Payload `{since, ts, issue, ref?}`, and CD-18 applies whole. The kind
  vocabulary is open: a reader renders an unknown kind by its `issue` line
  and never drops it. Recorded as CD-18's second family.
- **`kdash:nas:{host}` v1.** Usage is per ZFS pool, measured as usable space
  after parity. Raw `alloc` is not used, because raidz parity is not data.
  Disks are listed whether or not they are in a pool. Every type-specific
  value is present-and-null (CD-27). `collected_ts` and `source_stale` are
  separate from `ts`, so a live producer republishing a dead collector's data
  reads as stale. The registry pins every field to its `privileged.json`
  source, which is what the WI asked for in "version or pin the fields".
  Dataset-level usage is left out until the panel design asks for it. Root
  filesystem and activity already ride kpidash-client's telemetry. Recorded
  as CD-27's second application.
- **`kdash:net:ack:{host}`**, `EX 86400`, applied on the panel. The record
  holds `lfc` and `rx_errors` at ack time, plus `by` and `ts`. The effective
  delta is `min(delta_24h, counter − ack)`, or the live counter after a
  reset. Everything it needs is already in v1, so no producer, cleo's
  included, has to read anything new. Recorded as a CD-27 addendum. The
  writer is `kdash-pub setex`, which already exists, so it needs no code.

### Where the brief was a no-op or already done

- **"Python/Rust allowlists" for 3641:** nothing to add. Both are
  namespace-level, and `kdash` is already in them. The only allowlist change
  this sprint is removing `kstudiodash`.
- **The rpi53 key deletion (3986, after landing): nothing to delete.** From
  kai I ran `kdash-pub scan 'kdash:panel*'` and `scan 'kdash:stale:kstudio:*'`
  and both came back empty. To make sure that meant absent and not
  unreachable, `kdash-pub check` round-tripped a PING, and positive controls
  in the same session listed `kdash:net:*`, `kdash:panelmode:rpidash2` and
  eight `kdash:stale:*` keys. Neither kstudio key exists on central. No
  deletion was needed or made.
- **The vendored C changes only in comments.** Two header doc examples now
  say `.app = "kpidash"`. So the kdeskdash and kpidash re-vendors need no
  redeploy.

## What shipped

- Both publisher allowlists drop `kstudiodash`. A test on each side now
  refuses `kstudiodash:anything`, and both tests were watched failing first.
- `scripts/check.py`: the dashboard-local Status column, as above.
  Negative-tested: a status of `gone` fails with the vocabulary error, and
  `live` with the allowlists unchanged fails the both-directions check.
- Three schemas, each with examples and counterexamples run by the CD-24
  validator: `kdash-update`, `kdash-nas`, `kdash-net-ack`.
- Three registry rows and sections, including the NAS field mapping table.
  Also the kstudio retirement notes on `panel`, the apttemps bands and
  CD-26's consumer, and the Reserved list.
- `docs/architecture.md`: the consumer table marks kstudiodash retired, plus
  the CD-18 amendment, the CD-27 addendum and CD-27's second application.
- `rules.md`, `README.md`, the roadmap and both orientation files no longer
  name kstudiodash as live.

## Repaired in passing

- `cmake/aarch64-toolchain.cmake` called x86_64 "(kstudio)". x86_64 is still
  a target (the build host's own), so the host name was dropped. Proved by the
  C build in `just check`.
- `examples/kdash_dump.c`'s usage comment pointed `KDASH_PANEL_HOST` at
  kstudio. It now says rpidash2.

## Cross-repo (after merge)

kdeskdash and kpidash consume this repo as the `lib/kdashdata` submodule, so
the re-vendor can only point at a commit that exists on `main`. That is a
post-merge step. Each repo gets the submodule bump and its own kstudio comment
fixes:

- kdeskdash: `CMakeLists.txt` 43/45, `deploy/kdeskdash.service` 57,
  `CLAUDE.md` 188.
- kpidash: `scripts/unit-lint.sh` 54.

## Follow-ups

None filed. Writers and readers for the three new contracts are already
tracked: k-homelab after korg:3627, kpidash WIs 3642 and 3956, and korg:4158
for the ack.
