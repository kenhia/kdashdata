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

# Where would this host publish, and can it? One command, both wrappers' answer.
pub-endpoint: (pub "--app" "kdashdata" "endpoint")

# Build the Python wheel (publishing it to the homelab store is a separate step)
pub-wheel:
    cd publishers/python && uv build

# ---------------------------------------------------------------------------
# Distribution — sprint 004. `kdash-pub` is exec'd from Claude Code hooks on
# kai, kubs0 and cleo, so it has to reach those hosts as a versioned artifact
# rather than as whatever happens to sit in this checkout's target/.
# ---------------------------------------------------------------------------

# Show the store label this checkout would publish under.
#
# build.rs emits the label verbatim as the second field, so this reads it
# rather than reassembling it — see the comment there for why that matters.
[doc("Show the store label this checkout would publish under")]
version:
    #!/usr/bin/env bash
    set -euo pipefail
    cargo build --release -q --manifest-path publishers/rust/Cargo.toml
    ./publishers/rust/target/release/kdash-pub --version | awk '{ print $2 }'

# Publish a release build to the homelab package store (kubsdb :4880).
#
# Two artifacts, ONE version. The Linux binary and the Windows binary are built
# from the same checkout, carry the same `--version` label (build.rs reads the
# same git state for both), and land in the same store directory under the same
# `SHA256SUMS`. Never publish them as two versions: a fleet that resolves
# `latest` differently per platform is a fleet that drifts.
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
[doc("Publish linux+windows binaries to the package store as one version")]
publish:
    #!/usr/bin/env bash
    set -euo pipefail
    if [[ -n "$(git status --porcelain)" ]]; then
        echo "publish: refusing to publish from a dirty tree — a published version must name a commit" >&2
        exit 1
    fi
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
    cargo build --release --manifest-path publishers/rust/Cargo.toml
    cargo build --release --manifest-path publishers/rust/Cargo.toml --target x86_64-pc-windows-gnu
    stamp="$(./publishers/rust/target/release/kdash-pub --version)"
    v="$(printf '%s\n' "$stamp" | awk '{ print $2 }')"
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
    arch="$(uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
    echo "==> publishing kdash-pub $v as $stamp (linux + windows)"
    d=$(ssh -n kubsdb mktemp -d)
    scp publishers/rust/target/release/kdash-pub kubsdb:"$d/kdash-pub-$arch"
    scp publishers/rust/target/x86_64-pc-windows-gnu/release/kdash-pub.exe kubsdb:"$d/kdash-pub-x86_64-windows.exe"
    ssh -n kubsdb "kpkg artifact $latest_arg kdash-pub $v $d/* && rm -rf $d"

# Deploy the store's latest to the Linux publisher hosts.
#
# `kdash-pub` is a single static binary at a fixed absolute path, which is
# exactly knarr's default "file" shape — no --dest, no --unit, no --shape.
# The path is a CONTRACT, not a convenience: kdeskdash's claude-pub.sh execs
# `/usr/local/bin/kdash-pub` directly, because a hook context's PATH is not
# the interactive one.
#
# Pass --version to pin a build, or --dry-run to see the plan.
#
# cleo is NOT here — knarr installs over ssh with `install -m 0755`, which is
# not the Windows shape. It is a separate recipe, `just deploy-cleo`, and
# `just deploy-all` runs both.
[doc("Deploy the store's latest to the Linux publisher hosts (kai, kubs0)")]
deploy *ARGS:
    knarr deploy kdash-pub --host kai,kubs0 {{ARGS}}

# Install on cleo from the store.
#
# knarr cannot do this — its install step is `install -m 0755` over ssh — so
# `scripts/install-cleo.ps1` stands in for a knarr feature. Read that file's
# header before extending it; the ceiling is deliberate, and knarr WI 1763
# tracks retiring both copies.
#
# The script is COPIED and then run, never inlined into a quoted ssh command:
# cleo's ssh session is PowerShell, so the local shell, the ssh argument and
# the remote shell would each get a say in how the source is parsed, and it
# fails in ways that look like content errors.
#
# Pass --version to pin a build, e.g. `just deploy-cleo -Version 0.1.0-abc1234`.
[doc("Deploy the store's latest to cleo (Windows)")]
deploy-cleo *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    remote='C:/Users/kenhi/AppData/Local/Temp/install-kdash-pub.ps1'
    scp -q scripts/install-cleo.ps1 "cleo:$remote"
    ssh -n cleo "powershell -NoProfile -ExecutionPolicy Bypass -File $remote {{ARGS}}"
    ssh -n cleo "cmd /c del \"${remote//\//\\}\"" || true

# Deploy to every claude-publisher host.
#
# All three in one recipe, because the failure this exists to prevent is
# deploying *most* of them. kpolice sprint 002 redeployed the two hosts knarr
# reaches and left cleo on a commit that no longer existed — and the
# verification could not catch it, because it only iterated the hosts knarr
# had touched. Never verify by iterating what you deployed; name the hosts.
[doc("Deploy to all three publisher hosts: kai, kubs0 (knarr) and cleo (Windows)")]
deploy-all *ARGS:
    #!/usr/bin/env bash
    set -euo pipefail
    just deploy {{ARGS}}
    just deploy-cleo
