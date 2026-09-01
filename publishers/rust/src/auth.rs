//! Where the Redis password comes from — CD-2's contract, and CD-12's answer
//! for the contexts systemd cannot reach.
//!
//! CD-2 says the password travels in `REDISCLI_AUTH` and nowhere else. That is
//! still true, and it is still the only variable anything here reads. What
//! CD-12 adds is a second **delivery** route for the same variable: a daemon
//! gets it from its unit's `EnvironmentFile=`, but a Claude Code hook is
//! exec'd by a process that inherits neither that nor an interactive shell's
//! environment — measured on kai, `REDISCLI_AUTH` is UNSET under the Claude
//! Code process and under `bash -lc` alike.
//!
//! So when the variable is absent, this reads the same 0600 env file the unit
//! would have read. Same variable, same krot entry, same value; a delivery
//! mechanism, not a second contract.
//!
//! A world- or group-readable secret file is **refused**, not used. Publishing
//! anyway would hide exactly the class of fault the August 2026 rotation
//! surfaced.

use std::fmt;
use std::path::{Path, PathBuf};

/// The one variable that carries the password (CD-2).
pub const AUTH_ENV: &str = "REDISCLI_AUTH";

/// Explicit override naming the env file to read. Set it and no other
/// candidate is considered.
pub const AUTH_FILE_ENV: &str = "KDASH_AUTH_FILE";

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum AuthError {
    /// The file exists but is readable by group or other.
    TooOpen {
        path: PathBuf,
        mode: u32,
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
            AuthError::TooOpen { path, mode } => write!(
                f,
                "{} is mode {:04o} — a secret file readable by group or other \
                 is refused, not used (chmod 600)",
                path.display(),
                mode
            ),
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
/// The shape is systemd's, because the file this reads is the one systemd
/// already reads: `KEY=value`, one per line, `#` comments, optional surrounding
/// quotes. The last assignment wins, as it does for systemd. An empty
/// assignment is "not set" rather than "the empty password".
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

/// The env files to try, most specific first.
///
/// The kpidash-client file is last and is not kdashdata's to own — but it is
/// already on every reporting host at 0600 and already on krot's
/// `rpi53-redis-password` consumer list, so honouring it is what makes CD-12
/// work on day one without minting anything or copying a secret around.
pub fn candidates() -> Vec<PathBuf> {
    if let Some(explicit) = std::env::var_os(AUTH_FILE_ENV).filter(|v| !v.is_empty()) {
        return vec![PathBuf::from(explicit)];
    }
    let config = std::env::var_os("XDG_CONFIG_HOME")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|home| PathBuf::from(home).join(".config")));
    let Some(config) = config else {
        return Vec::new();
    };
    vec![
        config.join("kdash/redis-auth.env"),
        config.join("kpidash-client/redis-auth.env"),
    ]
}

/// Refuse a secret file anything but its owner can read.
#[cfg(unix)]
fn check_mode(path: &Path) -> Result<(), AuthError> {
    use std::os::unix::fs::PermissionsExt;
    let metadata = std::fs::metadata(path).map_err(|e| AuthError::Unreadable {
        path: path.to_path_buf(),
        detail: e.to_string(),
    })?;
    let mode = metadata.permissions().mode() & 0o777;
    if mode & 0o077 != 0 {
        return Err(AuthError::TooOpen {
            path: path.to_path_buf(),
            mode,
        });
    }
    Ok(())
}

/// Windows has no mode bits to check; Git Bash publishers rely on the ACLs the
/// profile directory already carries.
#[cfg(not(unix))]
fn check_mode(_path: &Path) -> Result<(), AuthError> {
    Ok(())
}

/// Read the password from one env file.
pub fn from_file(path: &Path) -> Result<String, AuthError> {
    check_mode(path)?;
    let text = std::fs::read_to_string(path).map_err(|e| AuthError::Unreadable {
        path: path.to_path_buf(),
        detail: e.to_string(),
    })?;
    parse_env_file(&text).ok_or_else(|| AuthError::NoValue {
        path: path.to_path_buf(),
    })
}

/// The password, or `None` when there is genuinely none to be had.
///
/// `Ok(None)` is a normal answer, not a failure: `rpidash2:6380` takes no AUTH
/// today, and a publisher there must work with nothing set. A file that exists
/// but cannot be trusted is the one case that errors — silence there would
/// turn a permissions fault into "the feed just stopped".
pub fn password() -> Result<Option<String>, AuthError> {
    if let Some(value) = std::env::var(AUTH_ENV).ok().filter(|v| !v.is_empty()) {
        return Ok(Some(value));
    }
    for path in candidates() {
        if !path.exists() {
            continue;
        }
        return from_file(&path).map(Some);
    }
    Ok(None)
}

#[cfg(test)]
mod tests {
    use super::*;

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

    #[cfg(unix)]
    #[test]
    fn a_group_readable_secret_file_is_refused() {
        use std::io::Write;
        use std::os::unix::fs::PermissionsExt;

        let dir = std::env::temp_dir().join(format!("kdash-pub-auth-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("redis-auth.env");
        let mut file = std::fs::File::create(&path).unwrap();
        writeln!(file, "REDISCLI_AUTH=secret").unwrap();
        drop(file);

        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o640)).unwrap();
        assert!(matches!(from_file(&path), Err(AuthError::TooOpen { .. })));

        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(from_file(&path).unwrap(), "secret");

        std::fs::remove_dir_all(&dir).ok();
    }
}
