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
//!
//! ## The commit is the last one touching THIS BINARY'S INPUTS, not `HEAD`
//!
//! Sprint 015, WI 2798, extending by analogy the rule Ken set on 2026-09-17
//! for klaude-top (WI 2782) and kdeskdash (WI 2801). The build is a release
//! `cargo build` of a fixed source tree, so a commit that touches only
//! `docs/`, `contracts/` or `sprints/` produces a byte-identical binary.
//! Stamping it with `HEAD` would publish that same binary under a new label
//! and churn the store's `latest` and every fleet install for no change —
//! which matters here more than most, because `.sprint-deploy` now runs
//! `publish` on *every* kdashdata ship, and most kdashdata sprints are
//! contract-only. Sprint 012 was exactly that.
//!
//! [`INPUTS`] is the source of that truth, and **the `justfile`'s `inputs`
//! variable must name the same paths**. Nothing enforces string equality
//! across the two — but `just publish` re-reads the built binary and refuses
//! to publish if its stamp and `just version` disagree, which is the same
//! drift caught one step later and with a message that says so.
//!
//! Note what is NOT here: `publishers/python/**` (a separate wheel, published
//! by a separate step, and its changes leave this binary identical), the
//! `tests/` tree, and the READMEs. And note what IS: `Cargo.toml`, so a
//! build-flag change in the `justfile` — which this list deliberately does not
//! watch — has a one-line remedy in bumping the crate version.

use std::path::{Path, PathBuf};
use std::process::Command;

/// The files that decide what the published binary contains, repo-root
/// relative. Keep in step with `inputs` in the repo's `justfile`.
const INPUTS: &[&str] = &[
    "publishers/rust/src",
    "publishers/rust/build.rs",
    "publishers/rust/Cargo.toml",
    "publishers/rust/Cargo.lock",
];

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
        if let Some(head) = read_git(&["rev-parse", "--symbolic-full-name", "HEAD"]) {
            let refpath = git_dir.join(&head);
            if refpath.exists() {
                println!("cargo:rerun-if-changed={}", refpath.display());
            }
        }
    }
    // And rerun when an input changes on disk, so the `-dirty` suffix appears
    // on the first build after an edit rather than after a `git add`.
    for input in ["src", "build.rs", "Cargo.toml", "Cargo.lock"] {
        println!("cargo:rerun-if-changed={input}");
    }

    let describe = input_commit().unwrap_or_else(|| "unknown".into());
    let date = read_git(&["log", "-1", "--format=%cd", "--date=short"])
        .unwrap_or_else(|| "unknown".into());
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

/// Short sha of the last commit touching [`INPUTS`], plus `-dirty` when the
/// working tree has uncommitted changes.
///
/// `None` outside a checkout, or when no commit touches the inputs at all —
/// which would be a repo this crate was copied into rather than a state to
/// paper over, so it degrades to `unknown` and `just publish` refuses it.
fn input_commit() -> Option<String> {
    let top = read_git(&["rev-parse", "--show-toplevel"])?;
    let mut args: Vec<&str> = vec!["-C", &top, "log", "-1", "--format=%h", "--"];
    args.extend_from_slice(INPUTS);
    let sha = read_git(&args)?;
    // The whole tree, not just the inputs: `just publish` refuses a dirty tree
    // outright, so this suffix is for the dev loop, where "is this binary the
    // one I just edited" is the question being asked.
    let dirty = read_git(&["status", "--porcelain"]).is_some();
    Some(if dirty { format!("{sha}-dirty") } else { sha })
}

/// Absolute path to the repository's git directory, or `None` outside a
/// checkout. `--git-dir` answers relative when run from the toplevel and
/// absolute otherwise, so both are handled.
fn git_dir() -> Option<PathBuf> {
    let dir = read_git(&["rev-parse", "--git-dir"])?;
    let path = Path::new(&dir);
    if path.is_absolute() {
        Some(path.to_path_buf())
    } else {
        Some(std::env::current_dir().ok()?.join(path))
    }
}

/// Run `git` and return its trimmed stdout, or `None` on failure or no output.
///
/// Takes the arguments already split, unlike the string-splitting version this
/// grew out of: `INPUTS` has to be spliced in, and a path is not something to
/// round-trip through `split(' ')`.
fn read_git(args: &[&str]) -> Option<String> {
    let out = Command::new("git").args(args).output().ok()?;
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
