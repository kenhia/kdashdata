# Sprint 017 — kdash-pub gains a darwin-arm64 build, built natively on a woken kimac

**Proposal:** korg:3146 (slice 5 of program korg:3148, "kimac: the fleet's
first Mac, fully onboarded")
**Covers:** WI 3139
**Branch:** `017-kdash-pub-darwin-arm64`
**Run as:** karc leg `kdashdata-288db7` on kai, overseen

## Goal

k-homelab's claude-hooks and copilot-hooks need `kdash-pub` on kimac (M1,
macOS 27). Ken ruled on 2026-09-22 (k-homelab WI 3123, decision 3a) that the
store gets Mac builds, built natively on kimac rather than cross-compiled
through osxcross. So `just publish` gains a third platform under the same
version, and `kdash-pub-arm64-darwin` is exactly the name knarr 0.6.0 asks for
when it deploys by the target's platform.

## The ruling that shaped it

WI 3139's rule 2 was "a sleeping kimac is skipped with an advisory". The
Wake-on-LAN natural-sleep run PASSed on 2026-09-23 at 08:36: the log names
`Enet.MagicPacket` as the wake reason, and the Mac woke 1 s after the packet.
It was a **dark** wake, though, and the Mac dropped back to sleep about 28 s
later. The `HOLD=1` confirmation run was never done. Ken ruled through the
overseer at 21:10 PDT (comment 2973): **build the wake path, not the skip
path**. Its sequence is packet, then ssh retry, then `caffeinate -u -t 2`, then
the build under `caffeinate -is`. This leg's own forced-sleep publish serves as
the missing HOLD confirmation. If kimac cannot be woken, linux and windows
still ship with an advisory. If the hold does not hold, the leg does not ship.

Scope fences from the overseer (comment 2971): leave `deploy` alone, because
WI 3094 owns that fold-back. Everything the turn owes runs in the foreground
(agent-skills WI 3126). Ken's `caffeinate` (PID 18590) is his and stays
untouched.

## Premise check

Run at 22:01 PDT on kai. All probes were run **from kai**, the host that does
the publish.

- **WI 3139: premise holds, with one drift.**
  - kpkg: `kpkg artifact [--force] [--no-latest] name version files…`, with no
    darwin awareness needed.
  - The store: `0.1.0-b73b5f4` is `latest` and holds linux, windows and
    `SHA256SUMS`.
  - kimac: `cargo 1.98.1`, host `aarch64-apple-darwin`. Command Line Tools are
    present, and `/bin/bash` is 3.2.57.
  - **Drift:** kimac cannot fetch the private `kenhia/khlenv` git dependency.
    `git ls-remote` fails with `could not read Username`, because kimac holds
    no GitHub credential. The WI's "sync the commit, `cargo build` there" would
    have failed at dependency resolution. The fix stays inside this repo: vendor
    on kai, and build `--offline` on the Mac (below). The direction is the same;
    this is a proceed.
- The WoL results log was read as the WI asks. The last line is the
  2026-09-23 08:36 natural PASS, with `DROPPED after 49s` and no HOLD run.
  That matches the overseer's summary.
- No cross-project plan applies: `kdashdata` is not in
  `cross-project-planning/index.md`.

## What was built

**`scripts/build-darwin.sh`** runs on kai:
`build-darwin.sh <version> <outdir>`. It exits 0 when built and stamp-checked,
**3** when kimac could not be woken and nothing was built, and 1 for anything
else.

1. **Local preparation happens before any packet goes out**, so the woken
   window is spent building:
   - a `git bundle` of HEAD's full history;
   - `cargo vendor --locked` (49 MB, 58 crates, khlenv included);
   - a tarball of both.
2. The magic packet, inlined from `kimac-wol-test/wake.py` rather than
   depending on that scratch dir: three rounds to ports 9 and 7 on
   192.168.1.255.
3. An ssh retry loop (`ConnectTimeout=20`, up to `WAKE_TIMEOUT=180`). **The
   probe command itself is `caffeinate -u -t 2`**, so the first successful
   answer is also the full-wake. `Host key verification failed` and
   `Permission denied` stop the loop at once as faults: the host answered, so
   they are not absence.
4. The payload is copied, then `ssh kimac "caffeinate -i -s /bin/bash -s -- …"`
   runs the remote half. The assertion lives exactly as long as the build, and
   nothing is left detached. ssh exit **255** is reported as "the hold did not
   hold", and any other non-zero is reported as a build failure.

**`scripts/build-darwin-remote.sh`** runs on the Mac, under bash 3.2. It checks
out the named sha from the bundle into a fresh repo. It pins the vendor config
to an absolute path and builds `--release --locked --offline` with
`CARGO_TARGET_DIR` **outside** the checkout, because a target dir inside it
would make build.rs stamp `-dirty`. It then compares the binary's own
`--version` against the label kai computed, and only after that emits
`kdash-pub-$(uname -m)-$(uname -s|lower)`.

**`justfile`:**
- `publish` builds darwin **last**, so a failed linux or windows build never
  costs a wake. Exit 3 prints
  `darwin: skipped, kimac unreachable — catch it up with: just publish-darwin <v>`
  and ships two. Any other failure uploads nothing. All the artifacts still go
  up in **one** `kpkg artifact` call. `--dry-run` names darwin.
- **`publish-darwin <version>`** is new. It refuses a dirty tree. It refuses a
  checkout whose `just version` is not `<version>`, and a version the store
  does not hold. It uses `just published`'s three outcomes, the same as
  `publish`, and uploads with `--no-latest` every time. An unwakeable kimac is
  a failure here, because asking for darwin by name means you believe it can
  be reached.
- `deploy` is untouched, per 2971.

**Docs:**
- CD-13 is amended (a new subsection, with the heading marked amended).
- `publishers/README.md` Distribution now describes the darwin build.
- A `build.rs` doc paragraph explains why the Mac derives the stamp from a
  bundle clone rather than being told it.
- Both orientation Status lines are updated.

### Decisions taken in the sprint

- **Vendor on kai rather than give kimac a GitHub credential.** A credential
  on the Mac would be a new secret to register and rotate, just so the Mac can
  fetch what kai already has. Vendoring also makes the Mac build `--offline`,
  and `--locked` holds it to the lockfile.
- **A git bundle, not `git archive`.** build.rs's stamp comes from
  `git log -- <inputs>`. An archive has no `.git`, so it would stamp `unknown`,
  which `publish` rightly refuses. The bundle carries full history, so `%h`
  abbreviates to the same length on both sides.
- **A mid-build failure is fatal and not a skip.** This is the same rule
  `deploy-all` applies to komarchy: a Mac that answered and then failed is a
  fault, not absence. Ken's ruling says a hold failure means "do not ship", and
  the recipe expresses that as "nothing uploads".

## Acceptance

_(filled in as each run completes)_

## Repaired in passing

## Follow-ups
