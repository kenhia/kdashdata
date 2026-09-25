# Windows: `just` runs recipes through `sh`, which Windows does not ship — put
# Git for Windows' `usr\bin` on PATH (it holds `sh.exe`) or run from Git Bash.
# (Upstream's own requirement: "sh must be available in the PATH".)

# List available recipes
default:
    @just --list

# Run every CI gate: docs, both publisher wrappers, then the C library and its tests
check: check-docs check-python check-rust build
    ctest --test-dir build --output-on-failure --no-tests=error

# Docs gate alone (python3 only): JSON parses, markdown links resolve, schemas registered
check-docs:
    @python3 scripts/check.py

# Python wrapper's pure core — stdlib only, so this runs with nothing installed
check-python:
    @PYTHONPATH=publishers/python/src python3 -m unittest discover -s publishers/python/tests

# Rust wrapper: format, lint, unit tests. Needs cargo, and — on a first build —
# network plus git access to the private khlenv repo (CD-11).
check-rust:
    cargo fmt --manifest-path publishers/rust/Cargo.toml --check
    cargo clippy --manifest-path publishers/rust/Cargo.toml --all-targets -- -D warnings
    cargo test --manifest-path publishers/rust/Cargo.toml

# Configure + build the native x86_64 tree: library, unit tests, kdash_dump
build:
    cmake -B build
    cmake --build build -j"$(nproc)"

# Run one C test by name, e.g. `just test keys`
test name: build
    ctest --test-dir build -R test_{{ name }} --output-on-failure --no-tests=error

# Cross-compile for the Pi dashboards (generic aarch64 — one build, every board)
build-aarch64:
    # Needs aarch64-linux-gnu-gcc and a Pi sysroot. kdeskdash's
    # `just sync-sysroot` populates one, and the same sysroot serves both repos.
    cmake -B build-aarch64 -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-toolchain.cmake
    cmake --build build-aarch64 -j"$(nproc)"

# Read every schema'd feed from the central Redis and print it (toy consumer)
dump *ARGS: build
    ./build/kdash_dump {{ ARGS }}

# The publisher CLI, release-built (the profile the hook path actually uses)
pub *ARGS:
    @cargo build --release --manifest-path publishers/rust/Cargo.toml --quiet
    ./publishers/rust/target/release/kdash-pub {{ ARGS }}

# Where would this host publish? Resolve and connect, and issue no command.
#
# Deliberately NOT the same question as `pub-check`: this one opens a socket
# and stops, and Redis only checks AUTH when AUTH is sent — so `--no-auth`
# exits 0 here against a Redis that requires a password (CD-25). Use it when
# "where" is the question; use `pub-check` when "can it" is.
pub-endpoint: (pub "--app" "kdashdata" "endpoint")

# ...and would the write be accepted? Round-trips a PING (sprint 016, CD-25).
#
# 0 accepted / 1 khlenv says this host publishes nowhere / 2 could not ask.
# This is the per-host check a rollout should run: it proves khlenv resolution
# AND the CD-12 auth route, which `pub-endpoint` cannot.
pub-check: (pub "--app" "kdashdata" "check")

# Build the Python wheel (publishing it to the homelab store is a separate step)
pub-wheel:
    cd publishers/python && uv build

# ---------------------------------------------------------------------------
# Distribution — sprint 004. `kdash-pub` is exec'd from Claude Code hooks on
# kai, kubs0 and cleo, so it has to reach those hosts as a versioned artifact
# rather than as whatever happens to sit in this checkout's target/.
#
# Sprint 015 made `publish` SELF-SKIPPING so `.sprint-deploy` can declare it
# (WI 2798). See `publishers/README.md`, "The version, and why it skips".
# ---------------------------------------------------------------------------

# The host holding the package store. Overridable so a test can point it
# somewhere harmless, which is also how `published`'s exit 2 was exercised.
store_host := env("KNARR_STORE_HOST", "kubsdb")

# The files that decide what the published BINARY contains.
#
# Not `HEAD`, and deliberately not everything: a commit touching only `docs/`,
# `contracts/` or `sprints/` produces a byte-identical binary, and stamping it
# with HEAD would republish it under a new label — churning the store's
# `latest` and every fleet install for no change. Most kdashdata sprints are
# contract-only, and `.sprint-deploy` now runs `publish` on every one of them.
#
# Not `publishers/python/**` either: that is a separate wheel with a separate
# publish step, and its changes leave this binary identical.
#
# `publishers/rust/build.rs` holds the same list as `INPUTS` and must stay in
# step with this one. Nothing compares the two strings — but `publish` re-reads
# the built binary and refuses if its stamp and `just version` disagree, which
# catches the drift one step later with a message that says so.
inputs := "publishers/rust/src publishers/rust/build.rs publishers/rust/Cargo.toml publishers/rust/Cargo.lock"

# Show the store label this checkout would publish under.
#
# Derived from git rather than from a built binary, so the publish predicate
# can ask "is this already in the store?" WITHOUT a cross-compile first — the
# no-op case must be cheap or nobody will leave it declared. `publish` closes
# the loop by asserting the built binary's own stamp equals this.
[doc("Show the store label this checkout would publish under")]
version:
    #!/usr/bin/env bash
    set -euo pipefail
    sha="$(git log -1 --format=%h -- {{inputs}})"
    if [[ -z "$sha" ]]; then
        echo "version: no commit touches the artifact inputs ({{inputs}}) — is this a git checkout?" >&2
        exit 1
    fi
    dirty=""
    if [[ -n "$(git status --porcelain)" ]]; then dirty="-dirty"; fi
    crate="$(sed -n 's/^version = "\(.*\)"/\1/p' publishers/rust/Cargo.toml | head -1)"
    printf '%s-%s%s\n' "$crate" "$sha" "$dirty"

# Is <version> already in the package store? THREE outcomes, never two.
#
#   0  present     1  absent     2  could not ask
#
# The remote answers with a WORD rather than with an exit code, and that is the
# whole point. A downed store host, a missing host key and a genuinely absent
# version all make `ssh … test -d` exit non-zero, and reading any of them as
# "absent" would republish over a store nobody could see — a false claim about
# the world, not a failed command. `publish` treats 2 as a refusal.
[doc("Is <version> already in the store? 0 = yes, 1 = no, 2 = could not ask")]
published version:
    #!/usr/bin/env bash
    set -uo pipefail
    remote='if [ -d "${KPKG_ROOT:-/datastore/packages}/artifacts/kdash-pub/{{version}}" ]; then echo present; else echo absent; fi'
    if ! ans="$(ssh -n -o BatchMode=yes -o ConnectTimeout=10 {{store_host}} "$remote" 2>&1)"; then
        echo "published: cannot reach {{store_host}} to ask about {{version}}: $ans" >&2
        exit 2
    fi
    case "$ans" in
        present) echo "present: {{version}} is in the store"; exit 0 ;;
        absent)  echo "absent: {{version}} is not in the store"; exit 1 ;;
        *)       echo "published: unexpected answer from {{store_host}}: ${ans:-(nothing)}" >&2; exit 2 ;;
    esac

# Publish a release build to the homelab package store (kubsdb :4880).
#
# Three artifacts, ONE version. The Linux binary and the Windows binary are
# built here, the darwin-arm64 binary natively on kimac (sprint 017), all from
# the same commit, carrying the same `--version` label (build.rs reads the same
# git state for each), and landing in the same store directory under the same
# `SHA256SUMS`. Never publish them as separate versions: a fleet that resolves
# `latest` differently per platform is a fleet that drifts.
#
# kimac is a Mac that sleeps. `scripts/build-darwin.sh` wakes it with a magic
# packet, holds it awake with caffeinate for the build, and brings the binary
# back. If it cannot be woken at all, the publish still ships linux + windows
# and PRINTS that darwin was skipped, with the `publish-darwin` line that
# catches it up — a sleeping Mac never blocks a publish (Ken, WI 3139 rule 2).
# If it wakes and the build then fails, that is a fault and nothing uploads.
#
# The binary is re-read with `--version` and published under the label that
# stamp produces, so the stamp and the store label are one fact rather than two
# that can drift — and that read is the same command knarr's confirm step runs
# on the target. The Windows binary cannot be executed here to be re-read, so
# it inherits the Linux binary's label; that is sound precisely because both
# come from one git state.
#
# A first build needs network and git access to the private khlenv repo (CD-11).
# That is a builder concern only — the deploy targets receive finished binaries.
[doc("Publish linux+windows+darwin binaries to the package store as one version")]
publish *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    dry=""
    for a in {{ARGS}}; do
        case "$a" in
            --dry-run) dry=1 ;;
            *) echo "publish: unknown argument '$a' (only --dry-run)" >&2; exit 2 ;;
        esac
    done
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "publish: refusing to publish from a dirty tree — a published version must name a commit" >&2
        exit 1
    fi
    # The no-op contract sprint-ship's Phase 7 expects of a declared publish
    # step: decide whether the artifact's inputs actually changed since the
    # last published version and, when they did not, do nothing and SAY so,
    # exiting 0. A contract-only sprint runs this and it does nothing, loudly —
    # which is what makes `recipe: publish` safe to declare unconditionally.
    #
    # `unknown` is not `absent`: refuse rather than publish blind.
    #
    # Output is captured because `just` prints its own "error: Recipe ...
    # failed" whenever a recipe exits non-zero, and here exit 1 is a normal
    # answer rather than a fault. The real diagnosis is re-emitted on the one
    # branch that is a fault.
    v="$(just version)"
    set +e
    ans="$(just published "$v" 2>&1)"
    rc=$?
    set -e
    case "$rc" in
        0) echo "nothing to publish: $v already in the store"; exit 0 ;;
        1) echo "publish: $v is not in the store — publishing" ;;
        *) echo "publish: could not determine whether $v is in the store — refusing to guess" >&2
           grep -v '^error: Recipe' <<<"$ans" >&2
           exit 1 ;;
    esac
    # Two separate prerequisites with two separate fixes, so say which is
    # missing rather than letting cargo report one confusing error for both.
    # kdashdata declares no rust-toolchain.toml (nothing here builds on a
    # second host), so the target is ambient and worth checking by name.
    if ! rustup target list --installed | grep -qx x86_64-pc-windows-gnu; then
        echo "publish: rustup target x86_64-pc-windows-gnu not installed — add it with:" >&2
        echo "           rustup target add x86_64-pc-windows-gnu" >&2
        exit 1
    fi
    if ! command -v x86_64-w64-mingw32-gcc >/dev/null; then
        echo "publish: x86_64-w64-mingw32-gcc not found — install it with:" >&2
        echo "           sudo apt install gcc-mingw-w64-x86-64" >&2
        exit 1
    fi
    if [[ -n "$dry" ]]; then
        echo "publish: --dry-run — would build linux + windows, wake kimac for darwin, and publish $v"
        echo "publish: would run: kpkg artifact$([[ "$(git rev-parse --abbrev-ref HEAD)" != "main" ]] && echo ' --no-latest') kdash-pub $v <linux> <windows> [<darwin>]"
        exit 0
    fi
    cargo build --release --manifest-path publishers/rust/Cargo.toml
    cargo build --release --manifest-path publishers/rust/Cargo.toml --target x86_64-pc-windows-gnu
    # The stamp and the store label are ONE fact. `just version` reassembles it
    # from git so the predicate above needs no build; build.rs derives it again
    # inside the binary. If the two lists ever drift apart, this is where it is
    # caught — before anything reaches the store, and with a message naming the
    # two places to reconcile.
    stamp="$(./publishers/rust/target/release/kdash-pub --version)"
    built="$(printf '%s\n' "$stamp" | awk '{ print $2 }')"
    if [[ "$built" != "$v" ]]; then
        echo "publish: the binary stamped '$built' but this checkout's label is '$v'." >&2
        echo "         the justfile's 'inputs' and build.rs's INPUTS have drifted apart." >&2
        exit 1
    fi
    case "$v" in
        *dirty*|*unknown*)
            echo "publish: binary stamped '$stamp' — that names no reproducible commit" >&2
            exit 1 ;;
    esac
    # A branch commit vanishes from history at squash-merge, so a branch build
    # may exist in the store to prove a path, but must never become what the
    # fleet resolves as `latest`.
    latest_arg=""
    if [[ "$(git rev-parse --abbrev-ref HEAD)" != "main" ]]; then
        latest_arg="--no-latest"
        echo "publish: not on main — publishing $v WITHOUT moving the latest pointer" >&2
    fi
    # darwin last, after everything local has succeeded, so a failed linux or
    # windows build never costs kimac a wake. Exit 3 is "could not be woken".
    darwin="$(mktemp -d)"
    trap 'rm -rf "$darwin"' EXIT
    set +e
    scripts/build-darwin.sh "$v" "$darwin"
    rc=$?
    set -e
    platforms="linux + windows + darwin"
    case "$rc" in
        0) ;;
        3) platforms="linux + windows"
           echo "darwin: skipped, kimac unreachable — catch it up with: just publish-darwin $v" >&2 ;;
        *) echo "publish: the darwin build failed — nothing uploaded" >&2; exit 1 ;;
    esac
    arch="$(uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
    echo "==> publishing kdash-pub $v as $stamp ($platforms)"
    d=$(ssh -n kubsdb mktemp -d)
    scp publishers/rust/target/release/kdash-pub kubsdb:"$d/kdash-pub-$arch"
    scp publishers/rust/target/x86_64-pc-windows-gnu/release/kdash-pub.exe kubsdb:"$d/kdash-pub-x86_64-windows.exe"
    for f in "$darwin"/kdash-pub-*; do
        if [[ -e "$f" ]]; then scp "$f" kubsdb:"$d/"; fi
    done
    ssh -n kubsdb "kpkg artifact $latest_arg kdash-pub $v $d/* && rm -rf $d"

# Add the darwin-arm64 binary to a version already in the store.
#
# The catch-up for a `publish` that printed "darwin: skipped". It builds from
# THIS checkout, so the checkout must be the commit <version> names — `just
# version` must equal it, which is checked rather than assumed — and it never
# moves `latest`: kpkg rewrites `SHA256SUMS` over the whole directory, and
# `--no-latest` leaves the pointer wherever it was.
#
# Unlike `publish`, an unwakeable kimac is a failure here: asking for the
# darwin build by name means you believe the Mac can be reached.
[doc("Add kdash-pub-arm64-darwin to an existing store version (wakes kimac; never moves latest)")]
publish-darwin version:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "publish-darwin: refusing to build from a dirty tree" >&2
        exit 1
    fi
    have="$(just version)"
    if [[ "$have" != "{{version}}" ]]; then
        echo "publish-darwin: this checkout builds '$have', not '{{version}}' — check out the commit it names" >&2
        exit 1
    fi
    set +e
    ans="$(just published "{{version}}" 2>&1)"
    rc=$?
    set -e
    case "$rc" in
        0) ;;
        1) echo "publish-darwin: {{version}} is not in the store — use 'just publish' for a new version" >&2; exit 1 ;;
        *) echo "publish-darwin: could not ask the store about {{version}}" >&2
           grep -v '^error: Recipe' <<<"$ans" >&2
           exit 1 ;;
    esac
    darwin="$(mktemp -d)"
    trap 'rm -rf "$darwin"' EXIT
    if ! scripts/build-darwin.sh "{{version}}" "$darwin"; then
        echo "publish-darwin: no darwin build for {{version}} — nothing uploaded" >&2
        exit 1
    fi
    echo "==> adding darwin to kdash-pub {{version}} (latest unchanged)"
    d=$(ssh -n kubsdb mktemp -d)
    scp "$darwin"/kdash-pub-* kubsdb:"$d/"
    ssh -n kubsdb "kpkg artifact --no-latest kdash-pub {{version}} $d/* && rm -rf $d"

# Deploy the store's latest to every publisher host.
#
# `kdash-pub` is a single static binary at a fixed absolute path, which is
# exactly knarr's default "file" shape — no --dest, no --unit, no --shape.
# The path is a CONTRACT, not a convenience: kdeskdash's claude-pub.sh execs
# `/usr/local/bin/kdash-pub` directly, because a hook context's PATH is not
# the interactive one (CD-13).
#
# All four publisher hosts in one call, because the failure this exists to
# prevent is deploying *most* of them. kpolice sprint 002 redeployed the two
# hosts knarr reached and left cleo on a commit that no longer existed — and
# the verification could not catch it, because it only iterated the hosts it
# had touched. Never verify by iterating what you deployed; name the hosts.
#
# - kai, kubs0: ordinary knarr hosts.
# - cleo: `--host-windows` — its ssh session is PowerShell, and knarr installs
#   to `C:\tools\bin\kdash-pub.exe` from here, verifying the store's SHA256
#   and keeping the old binary as `.prev` (knarr WI 1763).
# - komarchy: `--host-optional` — the laptop, asleep most of the time. If it
#   does not answer it is SKIPPED by name, and the run still exits 0; if it
#   answers and then fails, that is an ordinary failure (knarr WI 2680). A
#   host key or auth refusal is a failure too, not an excused absence. The
#   daily SKIPPED line is intended (ruled on korg WI 3094, comment 2910).
#
# Pass --version to pin a build, or --dry-run to see the plan.
[doc("Deploy the store's latest to kai, kubs0, cleo (Windows) and komarchy (skipped if asleep)")]
deploy *ARGS:
    knarr deploy kdash-pub --host kai,kubs0 --host-windows cleo --host-optional komarchy {{ARGS}}
