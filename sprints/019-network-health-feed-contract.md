# Sprint 019 — The network-health feed contract: `kdash:net:{host}` v1

**Proposal:** korg:4065 (slice 2 of program korg:4068, "Network-health card on kpidash")
**Covers:** WI 4060, plus WI 4003 repaired in passing
**Branch:** `019-network-health-feed-contract`
**Run as:** karc leg `kdashdata-8f26d7` on kai, overseen

## Goal

Write the Redis contract for the network-health card before anyone builds
either end. The card is kpidash's 2×1 "B · Rail", which replaces Fortune on
rpi53. The design was locked in slice 1 (korg:4059), and its field list is on
WI 4060. The overseer's brief was to version those fields, not redesign them,
and to take five defaulted rulings.

## Premise check

Run from kai at 18:07 PDT.

- **4060 holds.** Nothing in `contracts/` names a network feed, and no `net`
  family exists in either publisher allowlist or the registry. The producer
  claim checks out: kpidash-client (`~/src/tools/kpidash/clients/kpidash-client`)
  writes `kpidash:client:*` through its own redis-py client with `ex=5` /
  `ex=15`, the host-card convention the brief says to reuse.
- **4003 holds, and it has grown.** `deploy` named kai, kubs0, cleo and
  komarchy. A k-homelab search through kaed found kdash-pub deployed one-off
  to kbook2, kimac and thrall-arch, as the WI says, and to **thrall** as well,
  which the WI expected to "join next".

No cross-project plan applies, because kdashdata is not in the planning
index.

## Decisions

All five of the brief's rulings and their reasoning are in CD-27. In short:

- **Key: `kdash:net:{host}`, not the WI's sketched `kpidash:net:<host>`.**
  `kpidash:*` is frozen (CD-3), and new shared feeds go in `kdash:`.
  Neither end exists yet, so the namespace costs nothing.
- **Version: v1, in the key.** A breaking change becomes
  `kdash:net:v2:{host}`. The extra segment makes a v1 reader skip it. This is
  korg:3955's rule, expressed with the mechanism rules.md already has.
- **(1) `drops` is a list of producer events**, unix seconds over the last
  hour, capped at 64.
- **(2) No uplink-load field.**
- **(3) Freshness uses the host-card convention**: expiring, about 5 s with
  `EX 15`, plus `ts`.
- **(4) cleo sends the same shape** with `bond: false` and `leg: "5g"`. Its
  `lfc` pair is **required and null**, never absent. A counterexample pins
  the absent form.
- **(5) No fabric key.** The panel derives uplink-lost from the per-host
  `reach` records.
- Two semantics the field list did not fix, now fixed: `lfc` is the
  **primary slave's** count, and `reach.peers` means **at least one** peer
  answered.
- **One field beyond the list: an optional `nic` label.** The locked design
  draws the NIC chipset beside each host name (the mock-up's `AQC113`,
  `RTL8126`), so the card does draw it. It is optional and display-only.
  Flagged for the overseer.
- **Producers write directly**, not through the wrappers. kpidash-client
  already holds a Redis client. kpidash-client-win's repo is only on cleo
  and was not read, so slice 3.5 confirms its route.
- **Not checkable by the gate:** "null exactly when `bond` is false" and
  "`speed_mbps` null exactly when `leg` is down". The validator has no
  conditionals (CD-24), so these are stated as prose producer obligations
  instead of extending the validator. Extending it for one pairing would
  widen the slice.

## What shipped

- `contracts/schemas/kdash-net.schema.json`: the v1 schema, with five
  examples (healthy 10G, a host flapping on its 1G backup, cleo, the
  uplink-lost view, link down) and six counterexamples (absent no-bond
  counters, uppercase `1G`, missing `reach.peers`, a millisecond drop stamp,
  a millisecond `ts`, a `-1` counter).
- `contracts/registry.md`: a `kdash:net:{host}` row (v1) in the kdash
  table, a family section, and the Reserved list updated.
- `docs/architecture.md`: CD-27.
- `justfile` `deploy` and `publishers/README.md`: every kdash-pub host
  (WI 4003, below).
- Status lines in `CLAUDE.md` and `.github/copilot-instructions.md`.

No code changed. No new kdash-pub version comes out of this, and
`.sprint-deploy`'s self-skipping `publish` should find nothing to ship.

## Repaired in passing

- **WI 4003: `just deploy` missed four kdash-pub hosts.** The fix adds
  `komarchy,kbook2,kimac,thrall,thrall-arch` as `--host-optional`. The
  recipe's own rationale ("deploying *most* of them" is the failure)
  already answered the WI's question. The check is `just deploy --dry-run`,
  run from kai, the host that deploys. It planned kai, kubs0, cleo, kimac
  and thrall, and skipped komarchy and kbook2 (ssh timeout) and thrall-arch
  (name did not resolve; thrall is currently booted into Ubuntu). Exit 0.

## Gates

`just check`. The negative test came first: with the schema in place and no
registry row, `check-docs` failed `no link to schemas/kdash-net.schema.json`
(exit 1). With the row, it is green, with 16 schemas validating their own
examples and counterexamples.

## For slice 3 (kpidash)

kpidash vendors this repo as the `lib/kdashdata` submodule, currently at
`bb29b69` (sprint 009). Bump it to this sprint's merge commit on `main` so
the schema is in-tree. Nothing in the C library changed, so the bump moves
no consumer-facing API.

## Deployed

2026-10-26 18:17 PDT. Merge `6a928e3` (PR #19), shipped from kai under the overseer's clearance (korg:4065 comment 3990).

- **`just publish`**: a deliberate no-op. It printed `nothing to publish: 0.1.0-0ea8e7c already in the store`. This is a contract-only sprint, and the binary's inputs are unchanged since `0ea8e7c`.
- **`just deploy`**: the first run of the widened host list, exit 0.
  - Installed and confirmed (`kdash-pub --version`): `0.1.0-0ea8e7c` on kai, kubs0, cleo, kimac and thrall.
  - Skipped by name as allowed-absent:
    - komarchy and kbook2 (ssh connect timed out)
    - thrall-arch (name did not resolve; the dual-boot desktop is up as thrall)
- **Nothing to verify live for the contract.** `kdash:net:*` has no writer yet; kpidash slice 3 (korg:4066) is the first.
