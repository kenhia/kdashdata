#!/bin/bash
# build-darwin-remote.sh -- the half of the darwin build that runs ON kimac.
#
# Not run by hand. `scripts/build-darwin.sh` (on the publishing host) copies a
# payload to <dir> and then feeds this file to
#
#     caffeinate -i -s /bin/bash -s -- <dir> <sha> <version>
#
# so the whole build runs under the power assertion that keeps a woken Mac from
# dropping back to sleep mid-build (CD-13, sprint 017).
#
# Written for /bin/bash 3.2, which is what macOS ships and what an ssh command
# gets: no associative arrays, no `mapfile`, no `${var,,}`. Check it with
# `/bin/bash -n` ON THE MAC after any edit -- a Linux bash 5 accepts things
# 3.2 refuses.
#
# The payload is:
#   src.bundle  -- a git bundle of the publishing checkout's history, so
#                  build.rs finds a real git repo and stamps the same label
#                  the publishing host computed (a `git archive` would stamp
#                  `unknown`, which publish refuses)
#   vendor/     -- `cargo vendor` of the lockfile, so the build needs no
#                  network and, in particular, no credential for the private
#                  khlenv repo (CD-11), which kimac does not hold
#   vendor.toml -- the source-replacement config `cargo vendor` printed
#
# Prints `BUILT <file>` as its last line on success. Exit status is the build's.
set -eu

dir="$1"; sha="$2"; version="$3"

export PATH="$HOME/.cargo/bin:$PATH"
cd "$dir"
tar xzf payload.tgz

# A fresh repo fetching exactly the commit asked for, rather than a clone of the
# bundle's HEAD: the commit is named, so there is nothing to infer.
git init -q src
git -C src fetch -q ../src.bundle "$sha"
git -C src -c advice.detachedHead=false checkout -q "$sha"

# `cargo vendor` wrote a relative `directory`; cargo resolves a --config file's
# paths against the cwd, so pin it absolute rather than depend on where we are.
sed "s|^directory = .*|directory = \"$dir/vendor\"|" vendor.toml > vendor-abs.toml

# The target dir lives OUTSIDE the checkout so build.rs's `git status` sees a
# clean tree (a `-dirty` stamp is refused), and persists between builds so a
# catch-up after a publish does not start cold.
export CARGO_TARGET_DIR="$HOME/Library/Caches/kdash-pub-build"
cargo build --release --locked --offline \
    --config "$dir/vendor-abs.toml" \
    --manifest-path src/publishers/rust/Cargo.toml

bin="$CARGO_TARGET_DIR/release/kdash-pub"
stamp="$("$bin" --version)"
built="$(printf '%s\n' "$stamp" | awk '{ print $2 }')"
if [ "$built" != "$version" ]; then
    echo "build-darwin-remote: the binary stamped '$built' but the publish asked for '$version'" >&2
    exit 1
fi

arch="$(uname -m)-$(uname -s | tr '[:upper:]' '[:lower:]')"
cp "$bin" "$dir/kdash-pub-$arch"
echo "build-darwin-remote: $stamp"
echo "BUILT kdash-pub-$arch"
