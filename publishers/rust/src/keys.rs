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
/// `kdash` is the namespace for new shared feeds; `kpidash`, `claude` and
/// `kvscf` are grandfathered families (CD-3); `ghcp` sits outside `kdash:` by
/// one named exception (CD-21) and mirrors `claude:session` field-for-field;
/// `kdeskdash` and `kstudiodash` are dashboard-local state, listed for
/// visibility and not schema-governed.
///
/// **`scripts/check.py` holds this array to registry.md in both directions**,
/// because prose and enforcement drifting apart is not hypothetical: sprint
/// 012 legalised the `ghcp` family in the schema, the registry and the rules,
/// nothing taught this array, and every `ghcp:*` write was refused until
/// sprint 013 (WI 2781). Neither repo's gate could see it, because no gate
/// compared the two sides.
pub const NAMESPACES: &[&str] = &[
    "kdash",
    "kpidash",
    "claude",
    "kvscf",
    "ghcp",
    "kdeskdash",
    "kstudiodash",
];

/// The glob metacharacters `scan` accepts inside a segment.
///
/// Redis patterns also understand `[abc]` classes and `\\` escapes. Both are
/// deliberately refused: a character class is a footgun in a one-line shell
/// argument, and neither buys a publisher anything the two here do not. The
/// rule a reader needs is "`*` and `?`, nothing else", which is short enough
/// to be right about.
pub const GLOB_CHARS: &[char] = &['*', '?'];

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum KeyError {
    Empty,
    TooLong(usize),
    EmptySegment,
    BadSegment(String),
    UnknownNamespace(String),
    /// A `scan` pattern whose FIRST segment is globbed. `*:*` would sweep
    /// every family on a shared Redis, which is not a publisher's read.
    GlobbedNamespace(String),
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
            KeyError::GlobbedNamespace(ns) => write!(
                f,
                "pattern namespace {ns:?} is globbed — name the family you are \
                 reading. A pattern that crosses families reads keys this \
                 publisher has no contract with, on a Redis it shares"
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

/// Validate a `scan` pattern (CD-14, amended).
///
/// The same grammar as [`check_key`] with `*` and `?` allowed **inside** a
/// segment, and with one extra rule that is the whole reason a pattern gets
/// its own function rather than a flag: **the namespace segment may not be
/// globbed.** `check_key` would refuse a pattern outright, and relaxing it to
/// let `*` through anywhere would legalise `*:*` — one argument that reads
/// every family on a Redis this repo shares with kvscf and the dashboards.
/// A publisher's read is a read of its own feed.
pub fn check_pattern(pattern: &str) -> Result<(), KeyError> {
    if pattern.is_empty() {
        return Err(KeyError::Empty);
    }
    if pattern.len() > KEY_MAX {
        return Err(KeyError::TooLong(pattern.len()));
    }

    for segment in pattern.split(':') {
        if segment.is_empty() {
            return Err(KeyError::EmptySegment);
        }
        if !pattern_segment_ok(segment) {
            return Err(KeyError::BadSegment(segment.to_string()));
        }
    }

    let namespace = pattern.split(':').next().unwrap_or_default();
    if namespace.contains(GLOB_CHARS) {
        return Err(KeyError::GlobbedNamespace(namespace.to_string()));
    }
    if !NAMESPACES.contains(&namespace) {
        return Err(KeyError::UnknownNamespace(namespace.to_string()));
    }
    Ok(())
}

/// [`token_ok`]'s charset plus [`GLOB_CHARS`]. Length is bounded the same way:
/// a pattern segment stands in for a token, so it is held to a token's limit.
fn pattern_segment_ok(segment: &str) -> bool {
    !segment.is_empty()
        && segment.len() <= TOKEN_MAX
        && segment
            .chars()
            .all(|c| c.is_ascii() && (c.is_ascii_alphanumeric() || "._-*?".contains(c)))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scan_patterns_glob_inside_a_segment() {
        // kmon's read (korg:2213): every deployer that has skipped this host.
        assert!(check_pattern("kdash:stale:komarchy:*").is_ok());
        assert!(check_pattern("kdash:stale:*:*").is_ok());
        assert!(check_pattern("claude:session:kai:*").is_ok());
        assert!(check_pattern("ghcp:session:*").is_ok());
        assert!(check_pattern("kdash:panel?:kai").is_ok());
        // A pattern with no glob at all is a key, and a key is a legal pattern.
        assert!(check_pattern("claude:limits").is_ok());
    }

    #[test]
    fn a_pattern_may_not_glob_the_family_it_reads() {
        // The one that matters: `*:*` on a Redis shared with kvscf and the
        // dashboards is not a publisher's read of its own feed.
        assert_eq!(
            check_pattern("*:*"),
            Err(KeyError::GlobbedNamespace("*".into()))
        );
        assert_eq!(
            check_pattern("*"),
            Err(KeyError::GlobbedNamespace("*".into()))
        );
        assert_eq!(
            check_pattern("kdas?:stale:*"),
            Err(KeyError::GlobbedNamespace("kdas?".into()))
        );
        assert_eq!(
            check_pattern("weather:*"),
            Err(KeyError::UnknownNamespace("weather".into()))
        );
    }

    #[test]
    fn a_pattern_is_otherwise_held_to_the_key_grammar() {
        assert_eq!(check_pattern(""), Err(KeyError::Empty));
        assert_eq!(check_pattern("kdash::*"), Err(KeyError::EmptySegment));
        assert_eq!(check_pattern("kdash:stale:"), Err(KeyError::EmptySegment));
        assert_eq!(
            check_pattern("kdash:has space:*"),
            Err(KeyError::BadSegment("has space".into()))
        );
        // Redis understands these; this grammar does not, on purpose.
        assert_eq!(
            check_pattern("kdash:stale:[ab]:*"),
            Err(KeyError::BadSegment("[ab]".into()))
        );
        assert_eq!(
            check_pattern("kdash:stale:a\\*:*"),
            Err(KeyError::BadSegment("a\\*".into()))
        );
    }

    #[test]
    fn check_key_still_refuses_every_glob() {
        // The two functions are separate so that this stays true: a WRITE
        // never takes a pattern, whatever `scan` is allowed to say.
        for globbed in ["kdash:stale:*", "kdash:panel?:kai", "*:*"] {
            assert!(
                matches!(check_key(globbed), Err(KeyError::BadSegment(_))),
                "{globbed} should not be a writable key"
            );
        }
    }

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
        // The key sprint 012 legalised and this side refused (WI 2781).
        assert!(check_key("ghcp:session:kai:abc-123").is_ok());
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
