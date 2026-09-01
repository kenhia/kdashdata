//! CD-4 endpoint discovery for publishers — the same walk `kdash_endpoint.c`
//! implements for consumers, spelled in Rust and delegated to the khlenv
//! client rather than re-derived (CD-11).
//!
//! The walk, for a stem with a legacy alias and a compiled-in default:
//!
//! 1. `$<STEM>` in the environment — an explicit override wins outright, so an
//!    install can always pin an endpoint.
//! 2. khlenv `<STEM>`. An explicit null here is [`Resolved::Nowhere`] and
//!    stops: "deliberately no endpoint" is an answer, not a reason to keep
//!    walking.
//! 3. khlenv `<LEGACY>`, on a **miss** only.
//! 4. the compiled-in default, when khlenv itself could not be reached.
//!
//! Callers resolve on **every** connect. That is what makes a moved Redis
//! propagate within one reconnect interval instead of needing a config edit on
//! every publisher host — and it is what makes the CD-7 cutover a store edit.
//!
//! khlenv never holds secrets (CD-2). The password comes from `REDISCLI_AUTH`
//! and nowhere else; see [`crate::auth`].

use std::borrow::Cow;
use std::fmt;

/// CD-4: the stem every new consumer and publisher resolves for the central
/// Redis.
pub const CENTRAL_STEM: &str = "KDASH_CENTRAL_REDIS";

/// The legacy alias for the same endpoint, kept until kpidash-client's
/// publishers migrate (CD-3/CD-4). Tried only when the new stem MISSES.
pub const CENTRAL_STEM_LEGACY: &str = "KPIDASH_REDIS";

/// Where the central Redis has lived since kpidash 001. Used only when khlenv
/// itself cannot be reached: falling back to it can never be worse than having
/// no resolver at all.
pub const CENTRAL_DEFAULT: &str = "rpi53:6379";

/// CD-7: the stem the claude-feed relocation flips. Interim value
/// `rpidash2:6380`; after the cutover it names the central Redis.
///
/// Deliberately **no compiled-in default** — see [`Stem::default_value`].
pub const CLAUDE_STEM: &str = "KDASH_CLAUDE_REDIS";

pub const REDIS_PORT_DEFAULT: u16 = 6379;

/// Longest endpoint value khlenv will hand back, matching `KDASH_ENDPOINT_MAX`
/// in the consumer library.
pub const ENDPOINT_MAX: usize = 256;

/// One stem and the walk that belongs to it.
///
/// `Cow` rather than `String` so the well-known stems stay `const` and a
/// CLI-supplied one still fits without leaking.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Stem {
    pub key: Cow<'static, str>,
    pub legacy: Option<Cow<'static, str>>,
    /// What to use when khlenv is unreachable, if anything.
    pub default_value: Option<Cow<'static, str>>,
}

impl Stem {
    /// The central Redis: legacy alias, and the historical default.
    pub const CENTRAL: Stem = Stem {
        key: Cow::Borrowed(CENTRAL_STEM),
        legacy: Some(Cow::Borrowed(CENTRAL_STEM_LEGACY)),
        default_value: Some(Cow::Borrowed(CENTRAL_DEFAULT)),
    };

    /// The claude family's home — **no default on purpose.**
    ///
    /// CD-4's default exists because "the central Redis has always been at
    /// rpi53:6379", and for a *reader* a stale-but-right guess beats nothing.
    /// The claude stem is the one being flipped (CD-7), so a compiled-in guess
    /// is wrong on one side of the cutover or the other — and a publisher that
    /// guesses wrong does not miss a sample, it writes a convincing one to the
    /// Redis nobody is reading. A dropped publish is the honest degradation
    /// (CD-6); a confidently misdirected one is not.
    pub const CLAUDE: Stem = Stem {
        key: Cow::Borrowed(CLAUDE_STEM),
        legacy: None,
        default_value: None,
    };

    /// Any other stem, resolved with no alias and no default. A stem nobody
    /// has decided a fallback for does not get one invented at the call site.
    pub fn plain(key: impl Into<Cow<'static, str>>) -> Stem {
        Stem {
            key: key.into(),
            legacy: None,
            default_value: None,
        }
    }

    /// The stem a `--stem` argument names, with the well-known walks attached.
    pub fn named(key: &str) -> Stem {
        match key {
            CENTRAL_STEM => Stem::CENTRAL,
            CLAUDE_STEM => Stem::CLAUDE,
            other => Stem::plain(other.to_string()),
        }
    }
}

/// Where a stem resolved to.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Resolved {
    At {
        host: String,
        port: u16,
    },
    /// khlenv holds an explicit null: intentionally no endpoint. Not an error,
    /// and not something to fall back from.
    Nowhere,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EndpointError {
    Malformed {
        value: String,
        why: &'static str,
    },
    /// khlenv could not be reached and the stem has no default to stand in.
    Unavailable {
        stem: String,
        detail: String,
    },
    /// The service answered, the store holds nothing, and there is no default.
    Miss {
        stem: String,
    },
    /// khlenv rejected the app or key name — a caller bug, which no default
    /// papers over.
    Rejected {
        stem: String,
        detail: String,
    },
}

impl fmt::Display for EndpointError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            EndpointError::Malformed { value, why } => {
                write!(f, "malformed endpoint {value:?} — {why}")
            }
            EndpointError::Unavailable { stem, detail } => write!(
                f,
                "khlenv unreachable resolving {stem}, and this stem has no \
                 compiled-in default: {detail}"
            ),
            EndpointError::Miss { stem } => {
                write!(f, "khlenv holds no value for {stem} at any level")
            }
            EndpointError::Rejected { stem, detail } => {
                write!(f, "khlenv rejected {stem}: {detail}")
            }
        }
    }
}

impl std::error::Error for EndpointError {}

/// What one khlenv lookup can say. The four cases are the protocol's four
/// answers, kept apart because the CD-4 walk branches on all of them.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Lookup {
    /// 200 — a value.
    Value(String),
    /// 204 — an explicit null.
    Null,
    /// 404 — nothing at any level of the walk.
    Miss,
    /// Unreachable, timed out, or an unusable reply.
    Unavailable(String),
    /// 400 — the app or key name was rejected.
    Rejected(String),
}

/// Split `host` or `host:port`.
///
/// Mirrors `kdash_parse_hostport` exactly, refusals included: an IPv6 literal
/// is rejected rather than mis-split, because the homelab addresses Redis by
/// name or by IPv4 and silently taking `::1` apart would be worse than saying
/// no.
pub fn parse_hostport(value: &str, defport: u16) -> Result<(String, u16), EndpointError> {
    let bad = |why: &'static str| EndpointError::Malformed {
        value: value.to_string(),
        why,
    };

    let text = value.trim();
    if text.is_empty() {
        return Err(bad("empty"));
    }
    if text.len() > ENDPOINT_MAX {
        return Err(bad("longer than khlenv will ever hand back"));
    }

    let (host, port) = match text.split_once(':') {
        None => (text, defport),
        Some((host, rest)) => {
            if rest.contains(':') {
                return Err(bad("looks like an IPv6 literal, which is not supported"));
            }
            let port: u32 = rest.parse().map_err(|_| bad("port is not a number"))?;
            if !(1..=65535).contains(&port) {
                return Err(bad("port is out of range"));
            }
            (host, port as u16)
        }
    };

    if host.is_empty() {
        return Err(bad("host is empty"));
    }
    Ok((host.to_string(), port))
}

/// The CD-4 walk, with khlenv and the environment supplied by the caller.
///
/// Split from [`resolve`] so the branching — the part that is actually easy to
/// get wrong — is unit-testable with no network and no process environment.
pub fn resolve_with<E, L>(stem: &Stem, env: E, mut lookup: L) -> Result<Resolved, EndpointError>
where
    E: Fn(&str) -> Option<String>,
    L: FnMut(&str) -> Lookup,
{
    // 1. An explicit environment override wins outright.
    if let Some(value) = env(&stem.key).filter(|v| !v.trim().is_empty()) {
        let (host, port) = parse_hostport(&value, REDIS_PORT_DEFAULT)?;
        return Ok(Resolved::At { host, port });
    }

    // 2. The stem itself, then 3. its legacy alias on a MISS only.
    let mut unavailable: Option<String> = None;
    let keys = std::iter::once(stem.key.as_ref()).chain(stem.legacy.as_deref());
    for key in keys {
        match lookup(key) {
            Lookup::Value(value) => {
                let (host, port) = parse_hostport(&value, REDIS_PORT_DEFAULT)?;
                return Ok(Resolved::At { host, port });
            }
            // Deliberate: do not fall back to anything, alias or default.
            Lookup::Null => return Ok(Resolved::Nowhere),
            Lookup::Miss => continue,
            Lookup::Unavailable(detail) => {
                unavailable = Some(detail);
                break;
            }
            Lookup::Rejected(detail) => {
                return Err(EndpointError::Rejected {
                    stem: key.to_string(),
                    detail,
                })
            }
        }
    }

    // 4. The compiled-in default, if this stem has one.
    match stem.default_value.as_deref() {
        Some(value) => {
            let (host, port) = parse_hostport(value, REDIS_PORT_DEFAULT)?;
            Ok(Resolved::At { host, port })
        }
        None => Err(match unavailable {
            Some(detail) => EndpointError::Unavailable {
                stem: stem.key.to_string(),
                detail,
            },
            None => EndpointError::Miss {
                stem: stem.key.to_string(),
            },
        }),
    }
}

/// The live walk: process environment, real khlenv.
pub fn resolve(app: &str, stem: &Stem) -> Result<Resolved, EndpointError> {
    let client = khlenv_client::Khlenv::new(app);
    resolve_with(
        stem,
        |key| std::env::var(key).ok(),
        |key| match client.get(key) {
            Ok(Some(value)) => Lookup::Value(value),
            Ok(None) => Lookup::Null,
            Err(error) => match error.kind() {
                khlenv_client::ErrorKind::Miss => Lookup::Miss,
                khlenv_client::ErrorKind::InvalidKey => Lookup::Rejected(error.to_string()),
                _ => Lookup::Unavailable(error.to_string()),
            },
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn no_env(_: &str) -> Option<String> {
        None
    }

    fn at(host: &str, port: u16) -> Resolved {
        Resolved::At {
            host: host.into(),
            port,
        }
    }

    #[test]
    fn hostport_splits_like_the_c_library() {
        assert_eq!(
            parse_hostport("rpi53", 6379).unwrap(),
            ("rpi53".into(), 6379)
        );
        assert_eq!(
            parse_hostport("rpidash2:6380", 6379).unwrap(),
            ("rpidash2".into(), 6380)
        );
        assert_eq!(
            parse_hostport("  rpi53:6379\n", 6379).unwrap(),
            ("rpi53".into(), 6379)
        );
        assert_eq!(
            parse_hostport("192.168.1.144:6380", 6379).unwrap(),
            ("192.168.1.144".into(), 6380)
        );

        for bad in [
            "",
            "   ",
            ":6379",
            "rpi53:",
            "rpi53:abc",
            "rpi53:0",
            "rpi53:65536",
            "::1",
        ] {
            assert!(
                parse_hostport(bad, 6379).is_err(),
                "{bad:?} should be refused"
            );
        }
    }

    #[test]
    fn an_env_override_wins_outright() {
        let resolved = resolve_with(
            &Stem::CENTRAL,
            |key| (key == CENTRAL_STEM).then(|| "elsewhere:6390".to_string()),
            |_| panic!("khlenv must not be asked when the env pins the endpoint"),
        )
        .unwrap();
        assert_eq!(resolved, at("elsewhere", 6390));
    }

    #[test]
    fn an_empty_env_override_is_not_an_override() {
        let resolved = resolve_with(
            &Stem::CENTRAL,
            |_| Some("   ".to_string()),
            |key| {
                assert_eq!(key, CENTRAL_STEM);
                Lookup::Value("rpi53:6379".into())
            },
        )
        .unwrap();
        assert_eq!(resolved, at("rpi53", 6379));
    }

    #[test]
    fn the_legacy_alias_is_tried_on_a_miss_only() {
        let mut asked = Vec::new();
        let resolved = resolve_with(&Stem::CENTRAL, no_env, |key| {
            asked.push(key.to_string());
            match key {
                CENTRAL_STEM => Lookup::Miss,
                _ => Lookup::Value("rpi53:6379".into()),
            }
        })
        .unwrap();
        assert_eq!(resolved, at("rpi53", 6379));
        assert_eq!(asked, vec![CENTRAL_STEM, CENTRAL_STEM_LEGACY]);
    }

    #[test]
    fn an_explicit_null_stops_the_walk() {
        let mut asked = Vec::new();
        let resolved = resolve_with(&Stem::CENTRAL, no_env, |key| {
            asked.push(key.to_string());
            Lookup::Null
        })
        .unwrap();
        // Not the legacy alias, and not the default: a null is an answer.
        assert_eq!(resolved, Resolved::Nowhere);
        assert_eq!(asked, vec![CENTRAL_STEM]);
    }

    #[test]
    fn an_unreachable_khlenv_falls_back_to_the_default_without_trying_the_alias() {
        let mut asked = Vec::new();
        let resolved = resolve_with(&Stem::CENTRAL, no_env, |key| {
            asked.push(key.to_string());
            Lookup::Unavailable("connection refused".into())
        })
        .unwrap();
        assert_eq!(resolved, at("rpi53", 6379));
        // The alias lives in the same store on the same service: if the
        // service is down, asking it twice is a second timeout, not a second
        // chance.
        assert_eq!(asked, vec![CENTRAL_STEM]);
    }

    #[test]
    fn the_claude_stem_refuses_to_guess() {
        let error = resolve_with(&Stem::CLAUDE, no_env, |_| {
            Lookup::Unavailable("timed out".into())
        })
        .unwrap_err();
        assert!(matches!(error, EndpointError::Unavailable { .. }));

        let error = resolve_with(&Stem::CLAUDE, no_env, |_| Lookup::Miss).unwrap_err();
        assert_eq!(
            error,
            EndpointError::Miss {
                stem: CLAUDE_STEM.into()
            }
        );
    }

    #[test]
    fn the_claude_stem_still_takes_an_env_override_and_a_store_value() {
        let resolved = resolve_with(
            &Stem::CLAUDE,
            |key| (key == CLAUDE_STEM).then(|| "rpidash2:6380".to_string()),
            |_| panic!("not asked"),
        )
        .unwrap();
        assert_eq!(resolved, at("rpidash2", 6380));

        let resolved = resolve_with(&Stem::CLAUDE, no_env, |_| {
            Lookup::Value("rpi53:6379".into())
        })
        .unwrap();
        assert_eq!(resolved, at("rpi53", 6379));
    }

    #[test]
    fn a_rejected_key_is_never_papered_over_by_the_default() {
        let error = resolve_with(&Stem::CENTRAL, no_env, |_| {
            Lookup::Rejected("bad app name".into())
        })
        .unwrap_err();
        assert!(matches!(error, EndpointError::Rejected { .. }));
    }

    #[test]
    fn a_store_value_that_is_not_an_endpoint_is_an_error_not_a_fallback() {
        let error = resolve_with(&Stem::CENTRAL, no_env, |_| {
            Lookup::Value("rpi53:not-a-port".into())
        })
        .unwrap_err();
        assert!(matches!(error, EndpointError::Malformed { .. }));
    }
}
