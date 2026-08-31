# Windows: `just` runs recipes through `sh`, which Windows does not ship — put
# Git for Windows' `usr\bin` on PATH (it holds `sh.exe`) or run from Git Bash.
# (Upstream's own requirement: "sh must be available in the PATH".)

# List available recipes
default:
    @just --list

# Run every CI gate: docs, then the C library and its unit tests
check: check-docs build
    ctest --test-dir build --output-on-failure --no-tests=error

# Docs gate alone (python3 only): JSON parses, markdown links resolve
check-docs:
    @python3 scripts/check.py

# Configure + build the native x86_64 tree: library, unit tests, kdash_dump
build:
    cmake -B build
    cmake --build build -j"$(nproc)"

# Run one test by name, e.g. `just test keys`
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
