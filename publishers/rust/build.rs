//! Embed the git commit and its date so `kdash-pub --version` can tell one
//! build from another.
//!
//! This is not cosmetic. `just publish` re-reads the built binary with
//! `--version` and publishes it under the label that stamp produces, so the
//! stamp and the store label are one fact rather than two that can drift —
//! and knarr's confirm step re-reads the *installed* binary the same way. A
//! `dirty` or `unknown` stamp is refused at publish time: a published version
//! must name a commit someone can check out.
//!
//! Lifted from kpolice's `build.rs` (which mirrors kaed's, korg #924),
//! including the degrade-to-`unknown` behaviour — building from a tarball with
//! no `.git` must still work, and failing a build over a missing hash would be
//! a poor trade.
//!
//! One difference from kpolice, and the reason this is not a copy: the crate
//! root is `publishers/rust/`, not the repo root, so `.git` is two levels up.
//! `git rev-parse --git-dir` is asked where it is rather than assuming `.git`.

use std::path::{Path, PathBuf};
use std::process::Command;

fn main() {
    // Rerun when HEAD moves or the index changes; otherwise cargo caches the
    // stamp from whichever commit happened to be checked out first, which is
    // exactly the staleness this is meant to cure.
    if let Some(git_dir) = git_dir() {
        for p in ["HEAD", "index"] {
            let path = git_dir.join(p);
            if path.exists() {
                println!("cargo:rerun-if-changed={}", path.display());
            }
        }
        // `HEAD` on a branch points at a ref whose file moves on commit.
        if let Some(head) = read_git("rev-parse --symbolic-full-name HEAD") {
            let refpath = git_dir.join(&head);
            if refpath.exists() {
                println!("cargo:rerun-if-changed={}", refpath.display());
            }
        }
    }

    let describe = read_git("describe --always --dirty").unwrap_or_else(|| "unknown".into());
    let date = read_git("log -1 --format=%cd --date=short").unwrap_or_else(|| "unknown".into());
    let crate_version = std::env::var("CARGO_PKG_VERSION").expect("cargo sets this");

    // The second field is the store label VERBATIM — `0.1.0-c6a9b1c` — not a
    // form something else has to re-derive. knarr proves a host runs what it
    // was told to by testing whether `--version` output CONTAINS the label it
    // deployed, so a `0.1.0 (c6a9b1c)` shape fails that check even on a
    // perfectly correct install.
    let full = if describe == "unknown" {
        format!("{crate_version}-unknown")
    } else {
        format!("{crate_version}-{describe} ({date})")
    };

    println!("cargo:rustc-env=KDASH_PUB_VERSION_FULL={full}");
}

/// Absolute path to the repository's git directory, or `None` outside a
/// checkout. `--git-dir` answers relative when run from the toplevel and
/// absolute otherwise, so both are handled.
fn git_dir() -> Option<PathBuf> {
    let dir = read_git("rev-parse --git-dir")?;
    let path = Path::new(&dir);
    if path.is_absolute() {
        Some(path.to_path_buf())
    } else {
        Some(std::env::current_dir().ok()?.join(path))
    }
}

fn read_git(args: &str) -> Option<String> {
    let out = Command::new("git").args(args.split(' ')).output().ok()?;
    if !out.status.success() {
        return None;
    }
    let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}
