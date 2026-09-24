//! Where the Redis password comes from — CD-2's contract, CD-12's delivery,
//! CD-19's per-host file.
//!
//! CD-2 says the password travels in `REDISCLI_AUTH` and nowhere else. That is
//! still true, and it is still the only variable anything here reads. What
//! CD-12 added is a second **delivery** route for the same variable: a daemon
//! gets it from its unit's `EnvironmentFile=`, but a Claude Code hook is
//! exec'd by a process that inherits neither that nor an interactive shell's
//! environment — measured on kai, `REDISCLI_AUTH` is UNSET under the Claude
//! Code process and under `bash -lc` alike.
//!
//! CD-19 puts the fleet's own per-host file at the front of that route:
//! `/etc/khomelab/secrets.env` on Linux, `%ProgramData%\khomelab\secrets.env`
//! on Windows, rendered by k-homelab from the age store. The per-user files
//! CD-12 shipped with are still read, and are now deprecated.
//!
//! Two rules are load-bearing rather than tidy, and both are properties of the
//! **source** a candidate came from rather than of its path:
//!
//! - **A secret file anything untrusted can read, or anything can write, is
//!   refused rather than used.** What counts as untrusted differs: a per-user
//!   file is ours and 0600, so any group or other bit is a fault; the per-host
//!   file is `root:khomelab 0640` and group read is the whole access
//!   mechanism. See [`Source::mode_mask`].
//! - **The per-host file is shared, and not ours.** Nine keys, eight hosts,
//!   rendered by another repo, each host's manifest granting a subset. So "I
//!   cannot read it yet" and "it does not carry my key" are states of the
//!   fleet, not faults — they keep looking. See [`Source::optional`].
//!
//! Nothing here prints. The library has never written to stderr (CD-10), so
//! [`resolve`] reports *where* the password came from and the executable does
//! the telling.

use std::ffi::OsString;
use std::fmt;
use std::path::{Path, PathBuf};

/// The one variable that carries the password (CD-2).
pub const AUTH_ENV: &str = "REDISCLI_AUTH";

/// Explicit override naming the env file to read. Set it and no other
/// candidate is considered.
pub const AUTH_FILE_ENV: &str = "KDASH_AUTH_FILE";

/// The per-host file k-homelab renders on Linux: `root:khomelab 0640`,
/// `KEY='value'` lines (CD-19). macOS uses the same path by Ken's same-path
/// ruling (k-homelab WI 3123). The `cfg(unix)` mode check below applies to it
/// unchanged, so the darwin build has no platform branch here.
pub const PER_HOST_FILE: &str = "/etc/khomelab/secrets.env";

/// The Windows variable naming the per-machine data directory. Read at RUN
/// TIME and never defaulted: `C:\ProgramData` is what it resolves to on Ken's
/// machines today, but a VM elsewhere routinely resolves it to another drive
/// (Ken, 2026-09-12). Unset means the rung is skipped, not guessed.
pub const PROGRAMDATA_ENV: &str = "ProgramData";

/// Where a candidate came from. It fixes two things a path cannot: the
/// permissions the file must have, and what an unusable one *means*.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Source {
    /// `$REDISCLI_AUTH` answered. Never a file, so the file policy below never
    /// applies to it.
    Environment,
    /// `$KDASH_AUTH_FILE` — a caller named this file explicitly.
    Override,
    /// The per-host file k-homelab renders (CD-19).
    PerHost,
    /// The per-user files CD-12 shipped with. Deprecated: the k-homelab
    /// changeover deletes them.
    PerUser,
}

impl Source {
    /// Mode bits that make a secret file from this source untrustworthy.
    ///
    /// A `PerUser` file is single-purpose, ours, and was minted 0600, so ANY
    /// group or other bit is a fault. The per-host file's published contract is
    /// `0640 root:khomelab`: group **read** is how a publisher reaches it at
    /// all, so refusing that would refuse the contract. What stays refused
    /// everywhere is group **write** — any `khomelab` member could change the
    /// password every host reads — and any **other** bit, which defeats the
    /// group entirely. `0640` passes; `0644` and `0660` do not.
    pub fn mode_mask(self) -> u32 {
        match self {
            Source::PerUser => 0o077,
            _ => 0o027,
        }
    }

    /// Whether an unusable file on this rung means "keep looking" rather than
    /// "stop".
    ///
    /// Only the per-host file, and only for the two outcomes [`resolve`] names:
    /// it is shared with nine keys across eight hosts and rendered by another
    /// repo, so not being able to read it and it not carrying our key are both
    /// states of the fleet. Measured 2026-09-12: `ken` is in `khomelab` on
    /// kubs0 and is **not** on kai, so a fatal reading here would have stopped
    /// kai's publishers between this slice and the changeover (korg:2436).
    ///
    /// A mode that cannot be trusted is *not* covered by this — that is a
    /// fault, and it still stops.
    pub fn optional(self) -> bool {
        matches!(self, Source::PerHost)
    }

    /// Answering from here still works and should stop: the k-homelab
    /// changeover deletes these files.
    pub fn deprecated(self) -> bool {
        matches!(self, Source::PerUser)
    }

    /// How to name this source to a human.
    pub fn label(self) -> &'static str {
        match self {
            Source::Environment => AUTH_ENV,
            Source::Override => AUTH_FILE_ENV,
            Source::PerHost => "per-host secrets file",
            Source::PerUser => "per-user env file (deprecated)",
        }
    }
}

/// One file to try, and the source whose rules it plays by.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Candidate {
    pub path: PathBuf,
    pub source: Source,
}

/// The password, and where it came from.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Resolution {
    pub password: String,
    pub source: Source,
    /// The file that answered, or `None` when `$REDISCLI_AUTH` did.
    pub path: Option<PathBuf>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AuthError {
    /// The file exists but its mode is not one this source may be trusted at.
    TooOpen {
        path: PathBuf,
        mode: u32,
        source: Source,
    },
    Unreadable {
        path: PathBuf,
        detail: String,
    },
    /// The file was read and holds no `REDISCLI_AUTH=` line.
    NoValue {
        path: PathBuf,
    },
}

impl fmt::Display for AuthError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            AuthError::TooOpen { path, mode, source } => {
                let want = match source {
                    Source::PerUser => "chmod 600",
                    _ => {
                        "0640 root:khomelab at most — group read is the access \
                          mechanism, group write and world read are not"
                    }
                };
                write!(
                    f,
                    "{} is mode {:04o} — a secret file this open is refused, \
                     not used ({want})",
                    path.display(),
                    mode
                )
            }
            AuthError::Unreadable { path, detail } => {
                write!(f, "{}: {detail}", path.display())
            }
            AuthError::NoValue { path } => {
                write!(f, "{} holds no {AUTH_ENV}= line", path.display())
            }
        }
    }
}

impl std::error::Error for AuthError {}

/// Pull `REDISCLI_AUTH` out of an `EnvironmentFile`-shaped text.
///
/// The shape is systemd's, because the files this reads are ones systemd
/// already reads: `KEY=value`, one per line, `#` comments, optional surrounding
/// quotes. The last assignment wins, as it does for systemd. An empty
/// assignment is "not set" rather than "the empty password".
///
/// k-homelab writes the per-host file as `KEY='value'` deliberately — single
/// quotes are the one form bash, systemd `EnvironmentFile=`, docker compose
/// `env_file:` and python-dotenv all read identically (handoff korg:2480) —
/// and that is already the quoting this strips.
pub fn parse_env_file(text: &str) -> Option<String> {
    let mut found = None;
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        // Tolerate the `Environment=REDISCLI_AUTH=…` spelling too: that is what
        // the pre-WI-255 units held, and the k-homelab recipe migrates it by
        // copying the line through verbatim.
        let line = line.strip_prefix("Environment=").unwrap_or(line);
        let Some(value) = line
            .strip_prefix(AUTH_ENV)
            .and_then(|r| r.strip_prefix('='))
        else {
            continue;
        };
        let value = value.trim();
        let value = value
            .strip_prefix('"')
            .and_then(|v| v.strip_suffix('"'))
            .or_else(|| value.strip_prefix('\'').and_then(|v| v.strip_suffix('\'')))
            .unwrap_or(value);
        if !value.is_empty() {
            found = Some(value.to_string());
        }
    }
    found
}

/// The files to try, most specific first, resolved against an arbitrary
/// environment.
///
/// Split out from [`candidates`] so the ordering, the `ProgramData` rung and the
/// override's exclusivity are all testable **without mutating the process
/// environment** — `cargo test` is multi-threaded, so a test that sets a
/// variable is a test that flakes its neighbours.
pub fn candidates_in<F>(env: F) -> Vec<Candidate>
where
    F: Fn(&str) -> Option<OsString>,
{
    let set = |key: &str| env(key).filter(|v| !v.is_empty());

    // Unchanged contract (sprint 003): an explicit override is EXCLUSIVE.
    // CD-19 adds rungs below it, not a fall-through out of it — a caller
    // naming one file means that file.
    if let Some(explicit) = set(AUTH_FILE_ENV) {
        return vec![Candidate {
            path: PathBuf::from(explicit),
            source: Source::Override,
        }];
    }

    let mut out = Vec::new();

    // The per-host file (CD-19). Driven by the environment rather than by
    // `cfg(windows)` on purpose: `ProgramData` is unset on Linux, so the
    // Windows rung costs nothing there, and pointing it at a temp directory is
    // what lets the gate prove nothing is hardcoded on the host `just check`
    // actually runs on (WI 2406's second acceptance criterion).
    if let Some(programdata) = set(PROGRAMDATA_ENV) {
        out.push(Candidate {
            path: PathBuf::from(programdata)
                .join("khomelab")
                .join("secrets.env"),
            source: Source::PerHost,
        });
    }
    out.push(Candidate {
        path: PathBuf::from(PER_HOST_FILE),
        source: Source::PerHost,
    });

    // The deprecated per-user files. The kpidash-client one is last and is not
    // kdashdata's to own — but it is already on every reporting host at 0600
    // and already on krot's `rpi53-redis-password` consumer list, which is what
    // made CD-12 work on day one without minting anything.
    let config = set("XDG_CONFIG_HOME")
        .map(PathBuf::from)
        .or_else(|| set("HOME").map(|home| PathBuf::from(home).join(".config")));
    if let Some(config) = config {
        out.push(Candidate {
            path: config.join("kdash/redis-auth.env"),
            source: Source::PerUser,
        });
        out.push(Candidate {
            path: config.join("kpidash-client/redis-auth.env"),
            source: Source::PerUser,
        });
    }

    out
}

/// The files to try, most specific first, against this process's environment.
pub fn candidates() -> Vec<Candidate> {
    candidates_in(|key| std::env::var_os(key))
}

/// Refuse a secret file this source may not be trusted at.
#[cfg(unix)]
fn check_mode(path: &Path, source: Source) -> Result<(), AuthError> {
    use std::os::unix::fs::PermissionsExt;
    let metadata = std::fs::metadata(path).map_err(|e| AuthError::Unreadable {
        path: path.to_path_buf(),
        detail: e.to_string(),
    })?;
    let mode = metadata.permissions().mode() & 0o777;
    if mode & source.mode_mask() != 0 {
        return Err(AuthError::TooOpen {
            path: path.to_path_buf(),
            mode,
            source,
        });
    }
    Ok(())
}

/// Windows has no mode bits to check; the per-host file there relies on the
/// ACLs `%ProgramData%\khomelab\` carries, and Git Bash publishers on the ones
/// the profile directory already has.
#[cfg(not(unix))]
fn check_mode(_path: &Path, _source: Source) -> Result<(), AuthError> {
    Ok(())
}

/// Read the password from one env file, under one source's rules.
pub fn from_file(path: &Path, source: Source) -> Result<String, AuthError> {
    check_mode(path, source)?;
    let text = std::fs::read_to_string(path).map_err(|e| AuthError::Unreadable {
        path: path.to_path_buf(),
        detail: e.to_string(),
    })?;
    parse_env_file(&text).ok_or_else(|| AuthError::NoValue {
        path: path.to_path_buf(),
    })
}

/// Walk a candidate list, first answer wins.
///
/// Separated from [`resolve`] so the skip-and-keep-looking rules are testable
/// against a temp directory without touching the process environment.
pub fn resolve_candidates(candidates: &[Candidate]) -> Result<Option<Resolution>, AuthError> {
    for candidate in candidates {
        if !candidate.path.exists() {
            continue;
        }
        match from_file(&candidate.path, candidate.source) {
            Ok(password) => {
                return Ok(Some(Resolution {
                    password,
                    source: candidate.source,
                    path: Some(candidate.path.clone()),
                }))
            }
            // A shared file we cannot read yet, or one that does not carry our
            // key, is the fleet mid-changeover rather than a fault — keep
            // looking. A mode that cannot be trusted still stops, on every
            // rung: silently *using* a world-readable fleet password is worse
            // than a publisher failing loudly.
            Err(AuthError::Unreadable { .. } | AuthError::NoValue { .. })
                if candidate.source.optional() => {}
            Err(error) => return Err(error),
        }
    }
    Ok(None)
}

/// The password and where it came from, resolved against an arbitrary
/// environment.
pub fn resolve_in<F>(env: F) -> Result<Option<Resolution>, AuthError>
where
    F: Fn(&str) -> Option<OsString>,
{
    let from_env = env(AUTH_ENV)
        .and_then(|v| v.into_string().ok())
        .filter(|v| !v.is_empty());
    if let Some(password) = from_env {
        return Ok(Some(Resolution {
            password,
            source: Source::Environment,
            path: None,
        }));
    }
    resolve_candidates(&candidates_in(&env))
}

/// The password and where it came from, or `None` when there is genuinely none
/// to be had.
///
/// `Ok(None)` is a normal answer, not a failure: `rpidash2:6380` takes no AUTH
/// today, and a publisher there must work with nothing set.
pub fn resolve() -> Result<Option<Resolution>, AuthError> {
    resolve_in(|key| std::env::var_os(key))
}

/// The password alone. [`resolve`] is the one to reach for when the caller can
/// report which file answered.
pub fn password() -> Result<Option<String>, AuthError> {
    Ok(resolve()?.map(|r| r.password))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    /// A fake environment. Every test builds its own, so none of them touch the
    /// process environment `cargo test`'s threads share.
    fn env_of(pairs: &[(&str, &str)]) -> impl Fn(&str) -> Option<OsString> {
        let map: HashMap<String, String> = pairs
            .iter()
            .map(|(k, v)| ((*k).to_string(), (*v).to_string()))
            .collect();
        move |key: &str| map.get(key).map(OsString::from)
    }

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("kdash-pub-auth-{}-{tag}", std::process::id()));
        std::fs::remove_dir_all(&dir).ok();
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn write(path: &Path, text: &str) {
        if let Some(parent) = path.parent() {
            std::fs::create_dir_all(parent).unwrap();
        }
        std::fs::write(path, text).unwrap();
    }

    #[cfg(unix)]
    fn chmod(path: &Path, mode: u32) {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode)).unwrap();
    }

    #[test]
    fn a_plain_assignment_is_read() {
        assert_eq!(
            parse_env_file("REDISCLI_AUTH=hunter2\n").as_deref(),
            Some("hunter2")
        );
    }

    #[test]
    fn quotes_comments_and_neighbours_are_handled() {
        let text = "# the fleet password (krot: rpi53-redis-password)\n\
                    OTHER=ignored\n\
                    REDISCLI_AUTH=\"quoted value\"\n";
        assert_eq!(parse_env_file(text).as_deref(), Some("quoted value"));
        assert_eq!(
            parse_env_file("REDISCLI_AUTH='single'\n").as_deref(),
            Some("single")
        );
    }

    #[test]
    fn the_per_host_files_single_quoted_shape_parses() {
        // k-homelab writes `KEY='value'` (handoff korg:2480), among nine keys.
        let text = "POSTGRES_PASSWORD='other'\n\
                    REDISCLI_AUTH='fleet password'\n\
                    HF_TOKEN='third'\n";
        assert_eq!(parse_env_file(text).as_deref(), Some("fleet password"));
    }

    #[test]
    fn the_old_systemd_unit_spelling_still_parses() {
        // What the pre-WI-255 units held, and what k-homelab's migration copies
        // through verbatim.
        assert_eq!(
            parse_env_file("Environment=REDISCLI_AUTH=fromunit\n").as_deref(),
            Some("fromunit")
        );
    }

    #[test]
    fn the_last_assignment_wins_as_it_does_for_systemd() {
        assert_eq!(
            parse_env_file("REDISCLI_AUTH=old\nREDISCLI_AUTH=new\n").as_deref(),
            Some("new")
        );
    }

    #[test]
    fn an_empty_or_absent_assignment_is_not_a_password() {
        assert_eq!(parse_env_file(""), None);
        assert_eq!(parse_env_file("REDISCLI_AUTH=\n"), None);
        assert_eq!(parse_env_file("REDISCLI_AUTH=   \n"), None);
        assert_eq!(parse_env_file("# REDISCLI_AUTH=commented\n"), None);
        // A neighbouring variable whose name merely starts the same way.
        assert_eq!(parse_env_file("REDISCLI_AUTH_OLD=x\n"), None);
    }

    // --- the candidate list (CD-19) ----------------------------------------

    #[test]
    fn the_per_host_file_comes_before_the_per_user_files() {
        let found = candidates_in(env_of(&[("HOME", "/home/someone")]));
        let paths: Vec<String> = found.iter().map(|c| c.path.display().to_string()).collect();
        assert_eq!(
            paths,
            vec![
                PER_HOST_FILE.to_string(),
                "/home/someone/.config/kdash/redis-auth.env".to_string(),
                "/home/someone/.config/kpidash-client/redis-auth.env".to_string(),
            ]
        );
        assert_eq!(found[0].source, Source::PerHost);
        assert!(found[1].source.deprecated());
        assert!(found[2].source.deprecated());
    }

    #[test]
    fn programdata_is_read_from_the_environment_and_never_hardcoded() {
        let found = candidates_in(env_of(&[
            ("ProgramData", "D:/machine-data"),
            ("HOME", "/home/someone"),
        ]));
        assert_eq!(
            found[0].path,
            PathBuf::from("D:/machine-data/khomelab/secrets.env")
        );
        assert_eq!(found[0].source, Source::PerHost);
    }

    #[test]
    fn an_absent_programdata_skips_that_rung_rather_than_defaulting() {
        let found = candidates_in(env_of(&[("HOME", "/home/someone")]));
        assert!(
            !found
                .iter()
                .any(|c| c.path.display().to_string().contains("ProgramData")),
            "an unset ProgramData must skip the rung, not guess C:\\ProgramData"
        );
        // And an empty one is unset, not the filesystem root.
        let empty = candidates_in(env_of(&[("ProgramData", ""), ("HOME", "/home/someone")]));
        assert_eq!(empty[0].path, PathBuf::from(PER_HOST_FILE));
    }

    #[test]
    fn an_explicit_override_is_still_exclusive() {
        let found = candidates_in(env_of(&[
            ("KDASH_AUTH_FILE", "/tmp/named.env"),
            ("ProgramData", "D:/machine-data"),
            ("HOME", "/home/someone"),
        ]));
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].path, PathBuf::from("/tmp/named.env"));
        assert_eq!(found[0].source, Source::Override);
    }

    #[test]
    fn xdg_config_home_still_wins_over_home() {
        let found = candidates_in(env_of(&[
            ("XDG_CONFIG_HOME", "/elsewhere/config"),
            ("HOME", "/home/someone"),
        ]));
        assert_eq!(
            found[1].path,
            PathBuf::from("/elsewhere/config/kdash/redis-auth.env")
        );
    }

    #[test]
    fn a_bare_environment_still_offers_the_per_host_file() {
        // No HOME at all — a systemd unit with an empty environment. The
        // per-host file is the whole point: it does not need one.
        let found = candidates_in(env_of(&[]));
        assert_eq!(found.len(), 1);
        assert_eq!(found[0].path, PathBuf::from(PER_HOST_FILE));
    }

    // --- the mode policy (CD-19) -------------------------------------------

    #[cfg(unix)]
    #[test]
    fn a_group_readable_per_user_file_is_refused() {
        let dir = tmpdir("per-user-mode");
        let path = dir.join("redis-auth.env");
        write(&path, "REDISCLI_AUTH=secret\n");

        chmod(&path, 0o640);
        assert!(matches!(
            from_file(&path, Source::PerUser),
            Err(AuthError::TooOpen { .. })
        ));

        chmod(&path, 0o600);
        assert_eq!(from_file(&path, Source::PerUser).unwrap(), "secret");

        std::fs::remove_dir_all(&dir).ok();
    }

    #[cfg(unix)]
    #[test]
    fn a_group_readable_per_host_file_is_accepted_but_a_wider_one_is_not() {
        let dir = tmpdir("per-host-mode");
        let path = dir.join("secrets.env");
        write(&path, "REDISCLI_AUTH='shared'\n");

        // 0640 root:khomelab is the published contract — group read is how a
        // publisher reaches it at all.
        chmod(&path, 0o640);
        assert_eq!(from_file(&path, Source::PerHost).unwrap(), "shared");
        // Stricter is fine too.
        chmod(&path, 0o600);
        assert_eq!(from_file(&path, Source::PerHost).unwrap(), "shared");

        // World-readable defeats the group entirely.
        chmod(&path, 0o644);
        assert!(matches!(
            from_file(&path, Source::PerHost),
            Err(AuthError::TooOpen { mode: 0o644, .. })
        ));
        // Group-writable lets any member change the password every host reads.
        chmod(&path, 0o660);
        assert!(matches!(
            from_file(&path, Source::PerHost),
            Err(AuthError::TooOpen { mode: 0o660, .. })
        ));

        chmod(&path, 0o640);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[cfg(unix)]
    #[test]
    fn a_too_open_per_host_file_stops_the_walk_rather_than_being_skipped() {
        // The one fault that is NOT "keep looking": using a world-readable
        // fleet password silently is worse than failing loudly.
        let dir = tmpdir("per-host-too-open");
        let shared = dir.join("secrets.env");
        let mine = dir.join("redis-auth.env");
        write(&shared, "REDISCLI_AUTH='shared'\n");
        write(&mine, "REDISCLI_AUTH=mine\n");
        chmod(&shared, 0o644);
        chmod(&mine, 0o600);

        let walked = resolve_candidates(&[
            Candidate {
                path: shared,
                source: Source::PerHost,
            },
            Candidate {
                path: mine,
                source: Source::PerUser,
            },
        ]);
        assert!(matches!(walked, Err(AuthError::TooOpen { .. })));

        std::fs::remove_dir_all(&dir).ok();
    }

    // --- the walk (CD-19) --------------------------------------------------

    #[test]
    fn the_per_host_file_wins_when_both_exist() {
        let dir = tmpdir("precedence");
        let shared = dir.join("secrets.env");
        let mine = dir.join("redis-auth.env");
        write(&shared, "REDISCLI_AUTH='from the per-host file'\n");
        write(&mine, "REDISCLI_AUTH=from the per-user file\n");
        #[cfg(unix)]
        {
            chmod(&shared, 0o640);
            chmod(&mine, 0o600);
        }

        let found = resolve_candidates(&[
            Candidate {
                path: shared.clone(),
                source: Source::PerHost,
            },
            Candidate {
                path: mine,
                source: Source::PerUser,
            },
        ])
        .unwrap()
        .unwrap();
        assert_eq!(found.password, "from the per-host file");
        assert_eq!(found.source, Source::PerHost);
        assert!(!found.source.deprecated());
        assert_eq!(found.path.as_deref(), Some(shared.as_path()));

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_per_host_file_we_cannot_read_is_skipped_not_fatal() {
        // The measured state on kai, 2026-09-12: the file is there and `ken` is
        // not in `khomelab`, so reading it fails EACCES. A directory stands in
        // for that here (EISDIR through the same `Unreadable`) because it is
        // deterministic whatever uid the gate runs as — the real EACCES is
        // verified live.
        let dir = tmpdir("unreadable");
        let shared = dir.join("secrets.env");
        std::fs::create_dir_all(&shared).unwrap();
        #[cfg(unix)]
        chmod(&shared, 0o750);
        let mine = dir.join("redis-auth.env");
        write(&mine, "REDISCLI_AUTH=fell through\n");
        #[cfg(unix)]
        chmod(&mine, 0o600);

        let found = resolve_candidates(&[
            Candidate {
                path: shared,
                source: Source::PerHost,
            },
            Candidate {
                path: mine,
                source: Source::PerUser,
            },
        ])
        .unwrap()
        .unwrap();
        assert_eq!(found.password, "fell through");
        assert!(found.source.deprecated());

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_per_host_file_without_our_key_is_skipped_not_fatal() {
        // Nine keys across eight hosts; a host's manifest may grant a subset.
        let dir = tmpdir("no-key");
        let shared = dir.join("secrets.env");
        let mine = dir.join("redis-auth.env");
        write(
            &shared,
            "POSTGRES_PASSWORD='not ours'\nHF_TOKEN='nor this'\n",
        );
        write(&mine, "REDISCLI_AUTH=fell through\n");
        #[cfg(unix)]
        {
            chmod(&shared, 0o640);
            chmod(&mine, 0o600);
        }

        let found = resolve_candidates(&[
            Candidate {
                path: shared,
                source: Source::PerHost,
            },
            Candidate {
                path: mine,
                source: Source::PerUser,
            },
        ])
        .unwrap()
        .unwrap();
        assert_eq!(found.password, "fell through");

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_per_user_file_without_our_key_is_still_fatal() {
        // That file exists for exactly one reason, so silence there would turn
        // a real fault into "the feed just stopped".
        let dir = tmpdir("per-user-no-key");
        let mine = dir.join("redis-auth.env");
        write(&mine, "# rotated away and never refilled\n");
        #[cfg(unix)]
        chmod(&mine, 0o600);

        assert!(matches!(
            resolve_candidates(&[Candidate {
                path: mine,
                source: Source::PerUser,
            }]),
            Err(AuthError::NoValue { .. })
        ));

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn finding_nothing_is_still_a_valid_answer() {
        let dir = tmpdir("nothing");
        let walked = resolve_candidates(&[
            Candidate {
                path: dir.join("secrets.env"),
                source: Source::PerHost,
            },
            Candidate {
                path: dir.join("redis-auth.env"),
                source: Source::PerUser,
            },
        ]);
        assert_eq!(walked.unwrap(), None);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn the_environment_variable_still_wins_over_every_file() {
        let found = resolve_in(env_of(&[
            ("REDISCLI_AUTH", "explicit"),
            ("KDASH_AUTH_FILE", "/does/not/exist.env"),
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(found.password, "explicit");
        assert_eq!(found.source, Source::Environment);
        assert_eq!(found.path, None);
        assert!(!found.source.deprecated());
    }

    #[test]
    fn an_empty_environment_variable_is_not_a_password() {
        let dir = tmpdir("empty-env");
        let named = dir.join("named.env");
        write(&named, "REDISCLI_AUTH=from the file\n");
        #[cfg(unix)]
        chmod(&named, 0o600);

        let found = resolve_in(env_of(&[
            ("REDISCLI_AUTH", ""),
            ("KDASH_AUTH_FILE", named.to_str().unwrap()),
        ]))
        .unwrap()
        .unwrap();
        assert_eq!(found.password, "from the file");
        assert_eq!(found.source, Source::Override);

        std::fs::remove_dir_all(&dir).ok();
    }
}
