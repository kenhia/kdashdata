# 004 — Distribute kdash-pub (store publish + fleet install)

korg: proposal 1764, work items 1761 (publish) and 1762 (install).
Slice 1.5 of the claude-feed relocation program (korg 1755). Overseen: an
overseer session on cleo reviews this sprint before and after the ship.

## Goal

Sprint 003 built `kdash-pub` and scoped no distribution, so for one sprint the
binary existed only in this checkout's `target/` on kai. The CD-7 cutover
(korg 1753) needs it on kai, kubs0 and **cleo**, and parked at its brief on
exactly that gap. This sprint closes it — no new mechanism, just the one the
fleet already settled: package store, knarr, and the cleo installer pattern.

## Premise check at the brief

Both items' claims held, with two drifts worth recording:

- **#1761 said `kdash-pub` already has a `--version` to re-read.** Half true.
  `--version` existed but printed `kdash-pub 0.1.0` from `CARGO_PKG_VERSION` —
  no `build.rs`, no git stamp. A label of `0.1.0` names no commit and collides
  on every publish, and kpolice's recipe refuses exactly that. So the sprint
  grew a `build.rs`. Direction unchanged, one file bigger.
- **#1762's overseer comment expected kubs0 to lack a CD-12 auth path.** The
  reasoning was that kubs0's `REDISCLI_AUTH` copy lives in
  `/etc/klams/monitor.env`, which is not a candidate. Measured: that file is
  absent (or unreadable), and `~/.config/kpidash-client/redis-auth.env` **does**
  exist on kubs0 at mode 0600 — which is CD-12's third candidate. The suspected
  gap looked already closed before the install even ran.

Everything else verified live on kai: no `publish` recipe in the justfile; no
`kdash-pub` in the store (`/artifacts/kdash-pub/latest` → 404); mingw, both
rustup targets and rustc 1.98.0 present; no `-sys` crate anywhere in
`Cargo.lock`, so the `-gnu` cross-build has no C dependency to satisfy.

## Decisions

**CD-13** ([architecture.md](../docs/architecture.md)) records the shape:
one version carrying two artifacts, the stamp and the store label as one fact,
and the install paths as a contract rather than a convenience.

Three smaller calls made here rather than in the doc:

1. **`build.rs` resolves `.git` by asking, not by assuming.** kpolice's copy
   hardcodes `.git/HEAD` because its crate root *is* the repo root. Here the
   crate lives at `publishers/rust/`, so the same literal paths would silently
   register no `rerun-if-changed` and cargo would cache the stamp from whatever
   commit happened to be checked out first — the exact staleness the file
   exists to cure, arriving as a wrong version label rather than an error.
   `git rev-parse --git-dir` answers instead.

2. **No `rust-toolchain.toml`.** kpolice declares one to make the Windows
   target a property of the repo rather than of the machine. That argument is
   sound there and buys less here: `just check-rust` and `just publish` both
   invoke cargo from the repo root with `--manifest-path`, and rustup resolves
   a toolchain file from the working directory, so a file at
   `publishers/rust/` would not be read at all and one at the root would pin
   the whole repo for a crate two levels down. kdashdata also has no CI
   workflow to keep in step and exactly one host that builds. The protection is
   kept where it actually fires: `just publish` checks for the rustup target
   *and* the mingw linker by name, each with its own install command, before
   cargo can produce a confusing error for either.

3. **A fourth docs gate: `.ps1` files must be pure ASCII.** `install-cleo.ps1`
   carries a long `.NOTES` block explaining that Windows PowerShell 5.1 reads a
   BOM-less script as the system ANSI codepage, so one UTF-8 em-dash inside a
   double-quoted string terminates it early and the file fails to parse with
   errors pointing dozens of lines away. The rest of this repo uses em-dashes
   freely — which is precisely why an editor or an agent will eventually put
   one there. A comment asking nicely does not survive that; a gate does.
   Negative-tested: an em-dash planted in the `.PARAMETER Dest` block failed
   the gate at the right line with the offending bytes named.

## What shipped

- `publishers/rust/build.rs` — git stamp, degrading to `unknown` outside a
  checkout so a tarball build still works. `--version` now prints
  `kdash-pub 0.1.0-<describe> (<date>)`, the second field verbatim.
- `justfile` — `version`, `publish`, `deploy`, `deploy-cleo`, `deploy-all`.
- `scripts/install-cleo.ps1` — the second copy of the kpolice installer in the
  homelab, which was the agreed trigger to ask knarr for real Windows support.
  That request is knarr WI 1763; this file is on its retire list and is
  deliberately no larger than knarr's own install step.
- `scripts/check.py` — the ASCII gate above.
- `docs/architecture.md` CD-13, `publishers/README.md` Distribution.

## Verified live — the branch build

The recipes were exercised before the ship rather than after it, so the sprint
lands with the whole path proven instead of merely written. `just publish` on
the branch produced `0.1.0-ac2cd56`, both artifacts under one `kpkg` version
and one `SHA256SUMS`, with `--no-latest` applied automatically off `main` and
confirmed afterwards (`/artifacts/kdash-pub/latest` still 404). Windows
cross-build: 6.8s, 623 KB PE32+. That pinned version was then installed on all
three hosts and checked **by naming them**, never by iterating what the runner
reached:

| host | path | `--version` | `--app kdashdata endpoint` |
|---|---|---|---|
| kai | `/usr/local/bin/kdash-pub` | exit 0 | exit 0, `rpi53:6379` |
| kubs0 | `/usr/local/bin/kdash-pub` | exit 0 | exit 0, `rpi53:6379` |
| cleo | `C:\tools\bin\kdash-pub.exe` | exit 0 | exit 0, `rpi53:6379` |

`endpoint` exiting 0 everywhere means khlenv resolution **and** the CD-12 auth
route both work on every publisher host, against the authenticated central
Redis — which is the per-host verification the CD-7 cutover needs and did not
have.

Two CD-12 findings fell out of that, recorded in full on korg 1762:

- **The suspected kubs0 gap does not exist.** `~/.config/kpidash-client/redis-auth.env`
  is present there at 0600 — CD-12's third candidate — and `REDISCLI_AUTH` is
  unset in a non-interactive ssh, so the file route is what answered.
- **cleo has no file fallback at all.** Its `REDISCLI_AUTH` is a *Machine*-scope
  environment variable, inherited by every process including a hook context, so
  the file chain is never reached — and neither candidate file exists there.
  It works, and it is a single point of failure the Linux hosts do not have. Not
  hardened here: minting a second copy of a secret against a hypothetical is the
  wrong trade, and the choice is better recorded than made silently.

Because the branch commit vanishes at squash-merge, the fleet runs a doomed
label until the post-merge publish from `main` moves `latest` and
`just deploy-all` re-installs it. That is the Deployed section below.

## Follow-ups

- knarr WI 1763 retires `scripts/install-cleo.ps1`, and this repo is its second
  requester. Nothing to do here until knarr ships Windows support.
- The Python wheel still has no publish path (`pub-wheel` builds, nothing
  publishes). Out of scope: no consumer needs it from the store yet.
