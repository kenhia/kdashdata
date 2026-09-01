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
