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

Every run was on kai, from the branch, so every version is `--no-latest`.
kimac was probed from kai, which is the host that sends the packet and runs
the build. `latest` stayed `0.1.0-b73b5f4` throughout.

| # | run | version | result |
|---|---|---|---|
| 1 | `sudo pmset sleepnow` at 22:08:35, silence, then `just publish` at 22:09:05 | `0.1.0-59d6150` | **3 binaries + SHA256SUMS**, exit 0, 29 s. **But kimac never slept**; see below. This counts as the *awake* case. |
| 2 | `KIMAC_HOST=192.0.2.1 KIMAC_MAC=02:…:00 WAKE_TIMEOUT=30 just publish` | `0.1.0-1d2709d` | **2 binaries**, `darwin: skipped, kimac unreachable — catch it up with: just publish-darwin 0.1.0-1d2709d`, exit 0 |
| 3 | `sleepnow` at 22:11:44, silence, then `just publish-darwin 0.1.0-1d2709d` at 22:12:14 | `0.1.0-1d2709d` | darwin **added**, `SHA256SUMS` covers 3, `latest` unmoved, exit 0, 22 s. **kimac never slept this time either.** |

Script-level negative tests:
- a TEST-NET host gives exit 3 and "nothing built";
- a wrong version gives exit 1, with the Mac-side stamp check naming both
  labels;
- a vendor run under `--quiet` failed offline on khlenv. That was fixed, and
  the vendor config is now asserted non-empty.

The caffeinate half did what it was designed to do, as kimac's own
`pmset -g log` shows:
- `caffeinate -u` created UserIsActive for 2 s.
- `caffeinate -is` created PreventSystemSleep, which lasted 14 s in both
  runs (22:09:18–22:09:32 and 22:12:20–22:12:34) and was released
  `ClientDied` as the build exited.
- Nothing was left behind.

### The forced sleep never reached sleep: the HOLD confirmation is still outstanding

In both runs, `pmset -g log` shows the same sequence:
1. `Display is turned off` at the `sleepnow`;
2. then, **2–3 s later**, a HID event: the Magic Keyboard (`TurnedOn
   UserIsActive … AppleHIDKeyboardEventDriverV2`, 22:08:38) in the first run
   and the Magic Trackpad (`… AppleMultitouchDevice`, 22:11:46) in the second;
3. then `Display is turned on`.

There is **no `Sleep` entry at all**. `HIDIdleTime` read 64 s at 22:12:52, so
there was one event right after each attempt and nothing since. The
2026-09-22 forced WoL runs ABORTed with "still answering 120 s after
sleepnow" twice, which fits the same thing.

So "stays up through the whole build" was shown only for a Mac that was
already awake. The dark-wake case that the hold exists for, where a magic
packet wakes the Mac and it re-sleeps after about 28 s, was **not**
exercised. Whether the HID event is a person at the Mac or the Bluetooth
peripherals firing on sleep is not something this leg can tell, and it is not
this leg's to change. Parked for a ruling; see the proposal thread.

### Rerun with Ken hands-off (comment 3000): the hold holds, but the packet wakes nothing

It was Ken at the Mac. The rerun used a fresh version, `0.1.0-b425da3`, from
an input commit to the `Cargo.toml` profile comment. linux + windows were
published first with kimac pointed away (the advisory path again). The Mac's
build cache was cleared before each sleep.

**Run 4:** `sleepnow` at 22:16:36, then 30 s of quiet, then
`just publish-darwin 0.1.0-b425da3`. Darwin was added, `SHA256SUMS` covers 3,
`latest` did not move, and it exited 0 in 22 s. In kimac's log:
- `Sleep` at 22:16:41;
- an **unexplained** `DarkWake … Enet.Service` at 22:16:54, which was not
  this leg's traffic;
- `Maintenance Sleep` at 22:17:07;
- then `DarkWake … Enet.Service/HID Activity` at 22:17:09, and
  `DarkWake to FullWake … due to HID Activity` (the `caffeinate -u`);
- PreventSystemSleep 22:17:12–22:17:27, `ClientDied` at build end.

The script probed ssh at the same instant it sent the packet, so the two
raced. That was fixed: `PROBE_DELAY` (5 s) now gives the packet the first
word.

**Run 5:** the wake-and-hold path alone, `scripts/build-darwin.sh
0.1.0-b425da3`, with no upload, since the version already carries darwin.
`sleepnow` at 22:18:15, then 40 s of quiet.

| time | kimac `pmset -g log` |
|---|---|
| 22:18:20 | `Sleep` (Software Sleep) |
| 22:18:41 | `DarkWake … Enet.Service`: not this leg's traffic |
| 22:18:54 | `Sleep` (Maintenance Sleep) |
| 22:18:58 | **packet sent**, then 5 s alone |
| 22:19:03 | `DarkWake … Enet.Service`: the first ssh probe |
| 22:19:13 | `DarkWake to FullWake … due to HID Activity` (`caffeinate -u`) |
| 22:19:16–22:19:30 | PreventSystemSleep, `ClientDied` at build end; exit 0 |

Against comment 3000's three criteria:
- **A `Sleep` entry before the packet:** passes.
- **The `-is` hold covers the whole build:** passes. The ssh session never
  dropped, and `-u` turned the dark wake into a full wake both times.
- **`Enet.MagicPacket` as the wake reason:** **fails**, twice. In run 5 the
  packet had 5 s alone, and nothing woke the Mac until the ssh SYN did. The
  morning WoL test woke at +1 s, but that Mac had been in deep idle for 597 s.
  Here each packet landed seconds after a `Maintenance Sleep`, in a Mac that
  some other host's traffic keeps dark-waking about every 20 s.

Parked for a ruling (proposal thread).

### Ruling: the hold is confirmed, and the packet is best-effort (comment 3003)

The overseer ruled option 1. Ken's ruling made the forced-sleep acceptance
**the HOLD confirmation**, and runs 4 and 5 both show it: a real `Sleep`, a
dark wake promoted to full wake by `caffeinate -u`, and `caffeinate -is` held
through the whole build without an ssh drop.

The `Enet.MagicPacket` criterion over-specified that ruling. The packet waking
a deep-idle kimac was already proven at 08:35. Tonight's Mac was seconds into
re-sleep from a maintenance wake, which is a different state rather than a
contradiction. The other dark wakes in the quiet windows are kimac's known
background traffic (46 overnight, WI 3123 comment 2966), not a finding.

`5951c2c` (the 5 s grace period before the probe) stays. The script header
and CD-13 now say the packet is best-effort.

### Branch versions left in the store: none is `latest`, and none is a release

Every one was published from this branch with `--no-latest`, so each names a
commit the squash merge will erase. They exist to prove the path, and the
store's `latest` is still `0.1.0-b73b5f4`. The ship's own `publish` from
`main` cuts the real release.

| version | contents | made by |
|---|---|---|
| `0.1.0-59d6150` | linux, windows, darwin | run 1 (awake full publish) |
| `0.1.0-1d2709d` | linux, windows, darwin | run 2 (advisory) + run 3 (catch-up) |
| `0.1.0-b425da3` | linux, windows, darwin | advisory publish + run 4 (forced-sleep catch-up) |

There are **three** branch versions, read from the store listing on kubsdb
after the ruling. The previous pause reply said "four", which was a miscount
by this leg, and comment 3003 carried it forward. `0.1.0-b73b5f4` is `latest`
and still has no darwin build. The ship's own `publish` from `main` cuts a new
version with all three platforms, so no catch-up of `b73b5f4` is needed.

## Repaired in passing

- `publishers/rust/src/auth.rs`: `PER_HOST_FILE`'s doc said the file is
  rendered "on Linux". It now records that macOS takes the same path, by Ken's
  same-path ruling (WI 3123), and that `cfg(unix)`'s mode check applies
  unchanged. `check-rust` is the gate. This was also the input change that gave
  runs 2 and 3 a fresh version.

## Cross-repo changes made

None. The agent-skills host-section wording (comment 2979) goes to
agent-skills as its own slice, filed by the overseer using the wording in
comment 3001 (ruling 3003).

## Follow-ups

None filed.
- The agent-skills wording is the overseer's to file (3003).
- The unexplained dark wakes are known background traffic (3003).
- `deploy` for kimac belongs to slice 6 (korg:3142) and the deploy fold-back
  to WI 3094. Both were out of scope by 2971.
