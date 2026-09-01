//! Pure key grammar for the publish side — the choke point contracts/rules.md
//! requires, applied before a key ever reaches Redis.
//!
//! The consumer library validates keys it *discovers* (`kdash_keys.h`); this
//! validates keys a publisher is about to *create*, against the same charset
//! and the same fixed-segment discipline. Both ends refusing the same key is
//! the point: a writer that can emit a key no reader will parse has published
//! nothing, loudly or quietly.
//!
//! No Redis, no sockets, no clock.

use std::fmt;

/// Host/session token contract (rules.md): `[A-Za-z0-9._-]`, 1..=63 chars.
pub const TOKEN_MAX: usize = 63;

/// Widest key any governed family produces. Generous rather than tight — the
/// limit exists to refuse something pathological, not to police length.
pub const KEY_MAX: usize = 512;

/// The namespaces registry.md knows about. A key outside them is off-contract
/// by rules.md ("a publisher writing a key with no schema in kdashdata is off
/// contract"), and refusing it here is the cheapest place to say so.
///
/// `kdash` is the namespace for new shared feeds; the next three are
/// grandfathered families (CD-3); the last two are dashboard-local state,
/// listed for visibility and not schema-governed.
pub const NAMESPACES: &[&str] = &[
    "kdash",
    "kpidash",
    "claude",
    "kvscf",
    "kdeskdash",
    "kstudiodash",
];

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum KeyError {
    Empty,
    TooLong(usize),
    EmptySegment,
    BadSegment(String),
    UnknownNamespace(String),
}

impl fmt::Display for KeyError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            KeyError::Empty => write!(f, "key is empty"),
            KeyError::TooLong(n) => write!(f, "key is {n} bytes, over the {KEY_MAX} limit"),
            KeyError::EmptySegment => write!(f, "key has an empty `:` segment"),
            KeyError::BadSegment(s) => write!(
                f,
                "key segment {s:?} is not [A-Za-z0-9._-] of 1..={TOKEN_MAX} chars"
            ),
            KeyError::UnknownNamespace(ns) => write!(
                f,
                "namespace {ns:?} is not one of {} — a feed with no schema in \
                 kdashdata is off-contract (contracts/rules.md)",
                NAMESPACES.join(", ")
            ),
        }
    }
}

impl std::error::Error for KeyError {}

/// True when `tok` satisfies the token contract: non-empty, <= 63 chars,
/// charset `[A-Za-z0-9._-]` only.
///
/// Mixed case is accepted deliberately, because the consumer library accepts
/// it: hostnames and session ids arrive from `hostname` and from Claude Code,
/// and a publisher stricter than its reader would refuse keys that work.
pub fn token_ok(tok: &str) -> bool {
    !tok.is_empty()
        && tok.len() <= TOKEN_MAX
        && tok
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'.' || b == b'_' || b == b'-')
}

/// Validate a key a publisher is about to write.
pub fn check_key(key: &str) -> Result<(), KeyError> {
    if key.is_empty() {
        return Err(KeyError::Empty);
    }
    if key.len() > KEY_MAX {
        return Err(KeyError::TooLong(key.len()));
    }

    for segment in key.split(':') {
        if segment.is_empty() {
            return Err(KeyError::EmptySegment);
        }
        if !token_ok(segment) {
            return Err(KeyError::BadSegment(segment.to_string()));
        }
    }

    // Namespaces are lowercase by rules.md, and the check is exact: `Claude`
    // and `claude` are different keys to Redis, so accepting either would put
    // two families in one namespace.
    let namespace = key.split(':').next().unwrap_or_default();
    if !NAMESPACES.contains(&namespace) {
        return Err(KeyError::UnknownNamespace(namespace.to_string()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tokens_follow_the_rules_md_charset() {
        assert!(token_ok("rpidash2"));
        assert!(token_ok("a"));
        assert!(token_ok("dev_telemetry"));
        assert!(token_ok("host.name-1"));
        assert!(token_ok("MixedCase")); // the reader accepts it; so do we
        assert!(token_ok(&"x".repeat(TOKEN_MAX)));

        assert!(!token_ok(""));
        assert!(!token_ok(&"x".repeat(TOKEN_MAX + 1)));
        assert!(!token_ok("has space"));
        assert!(!token_ok("has:colon"));
        assert!(!token_ok("sl/ash"));
        assert!(!token_ok("qu\"ote"));
        assert!(!token_ok("new\nline"));
    }

    #[test]
    fn governed_keys_pass() {
        assert!(check_key("kdash:demo:thing").is_ok());
        assert!(check_key("kpidash:services:kdashdata-demo:kai").is_ok());
        assert!(check_key("kpidash:services:sonarr:_").is_ok());
        assert!(check_key("claude:session:kai:abc-123").is_ok());
        assert!(check_key("claude:limits").is_ok());
        assert!(check_key("kvscf:instances:cleo").is_ok());
        assert!(check_key("kdeskdash:active_mode").is_ok());
    }

    #[test]
    fn an_unknown_namespace_is_off_contract() {
        assert_eq!(
            check_key("weather:now"),
            Err(KeyError::UnknownNamespace("weather".into()))
        );
        // Case matters: Redis would treat this as a separate family.
        assert_eq!(
            check_key("Claude:limits"),
            Err(KeyError::UnknownNamespace("Claude".into()))
        );
    }

    #[test]
    fn malformed_keys_are_refused_not_trimmed() {
        assert_eq!(check_key(""), Err(KeyError::Empty));
        assert_eq!(check_key("kdash::thing"), Err(KeyError::EmptySegment));
        assert_eq!(check_key("kdash:"), Err(KeyError::EmptySegment));
        assert_eq!(check_key(":kdash"), Err(KeyError::EmptySegment));
        assert_eq!(
            check_key("kdash:has space"),
            Err(KeyError::BadSegment("has space".into()))
        );
        let long = format!("kdash:{}", "x".repeat(KEY_MAX));
        assert!(matches!(check_key(&long), Err(KeyError::TooLong(_))));
    }
}
