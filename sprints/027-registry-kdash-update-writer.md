# Sprint 027 — `kdash:update` has its writer

**Proposal:** korg:4824 (relay from k-homelab sprint 132, korg:4822; exact
text in handoff korg:4823, "Relays" item 1)
**Covers:** no work item (a finding repaired in the same run)
**Branch:** `027-registry-kdash-update-writer`
**Run as:** karc leg `kdashdata-bac8db` on kai, overseen

## Goal

Sprint 020 registered `kdash:update:{host}:{kind}` code-free. Its registry
entry still said **Writer, not yet** and listed four kinds. k-homelab sprint
132 shipped the writer on 2026-11-18 (`bin/update-flag`), which raises six
kinds. The goal was to bring the contract's prose in line with that.

## Premise check

Run on kai, 2026-11-18 18:05 PST.

- **Holds.** k-homelab `origin/main` (read via `gh api`, not a host checkout)
  has `5837865 feat(132): apt ring card on kdash:update…`. Its
  `bin/update-flag` writes `kdash:update:<host>:<kind>`.
- The relay's Reader sentence was checked too. kpidash `origin/main` has
  `sprints/028-update-card-nas-panel-khlenv-retry.md` and CLIENT-PROTOCOL
  §8e "Update-Issue Cards (Sprint 028, WI #3642)".
- No guiding plan lists kdashdata.

## What changed

- `contracts/registry.md`, the `kdash:update` section, has three changes:
  - The Writer/Reader paragraph now names `bin/update-flag` and kpidash's card.
  - The "Known kinds" line gains `approval-awaiting` and `overdue`.
  - The table row drops "(not yet — see below)".
- `contracts/schemas/kdash-update.schema.json`: only the `description`'s list
  of kinds changed. No constraint changed and the vocabulary stays open.

One deviation from the relay's instructions: it said to replace the paragraph
"through `…so no host is ever flagged twice.`". Its replacement text carries
its own **Reader** sentence, though. Stopping there would have left the old
"Reader … not yet built" lines below it, contradicting the new one. So the
whole Writer/Reader paragraph was replaced, and its still-true last sentence
("No C reader ships with this contract.") was kept.

No CD amendment was needed. CD-18's sprint-020 addendum fixes the open
vocabulary and does not enumerate kinds. The rules add nothing for a new kind.

## Repaired in passing

- `sprints/planning/roadmap.md` Next still listed the k-homelab writer move and
  kpidash's update card (WI 3642) as pending. Both have shipped, so the bullet
  now keeps only the NAS and ack items. The `check-docs` link gate covers it.
- The Status line "Next" in both orientation files listed "the writers and cards
  for the update and NAS feeds". It now names only the NAS feed. `check-docs`'
  Status gate covers both files.

## Not in scope

The vendored copies in kpidash and kdeskdash `lib/kdashdata` follow on their
next sync. kmon's dead `APT_UPDATE_DEPLOYER` exclusion is relay 2 of the same
handoff and belongs to kmon.

## Deployed

Merged as `ce59b4e` (PR #28), 2026-11-18 18:05 PST, from karc leg
`kdashdata-bac8db` on kai. The deploy steps came from `.sprint-deploy` and ran
from merged `main`:

- **`just publish`**: a deliberate no-op. The recipe printed "nothing to publish:
  0.1.0-4837473 already in the store". This sprint left the `kdash-pub` inputs
  untouched.
- **`just deploy`**: rc 0. knarr installed `kdash-pub 0.1.0-4837473`, the
  unchanged `latest`, on kai, kubs0, cleo and thrall. It skipped komarchy,
  kbook2 and kimac (ssh timed out) and thrall-arch (its name did not resolve).
  All four are `--host-optional`, so the skips are normal outcomes.

There is nothing live to smoke-test. The sprint changed contract prose and a
schema `description`, which no binary reads.
