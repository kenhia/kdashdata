//! The publish verbs, parsed and validated before anything opens a socket.
//!
//! Both front ends go through here — the crate's typed methods and the CLI's
//! argv — so a key that fails the grammar or a payload that fails the `ts`
//! rule is refused in the same place, whichever end asked. That is CD-10's
//! pure-core/I-O-shell split applied to the publish side: everything easy to
//! get wrong is decided here, with no clock and no network, and the shell just
//! writes what it is handed.

use crate::keys::{self, KeyError};
use crate::payload::{self, PayloadError};
use std::fmt;

/// One Redis write, already validated and stamped.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    /// Latest-value, ts-owned (rules.md): STRING, no TTL, `ts` required.
    SetLatest {
        key: String,
        payload: String,
    },
    /// Latest-value, expiring (rules.md): STRING with `SET … EX <ttl>`.
    SetExpiring {
        key: String,
        ttl: u64,
        payload: String,
    },
    /// A HASH record — the shape the grandfathered `claude:*` family uses.
    HSet {
        key: String,
        pairs: Vec<(String, String)>,
    },
    Expire {
        key: String,
        ttl: u64,
    },
    Del {
        key: String,
    },
    /// Event log, capped: the LPUSH half. The writer owns the cap, so the
    /// LTRIM is a separate command it must send itself.
    LPush {
        key: String,
        payload: String,
    },
    LTrim {
        key: String,
        start: i64,
        stop: i64,
    },
}

impl Command {
    pub fn key(&self) -> &str {
        match self {
            Command::SetLatest { key, .. }
            | Command::SetExpiring { key, .. }
            | Command::HSet { key, .. }
            | Command::Expire { key, .. }
            | Command::Del { key }
            | Command::LPush { key, .. }
            | Command::LTrim { key, .. } => key,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ParseError {
    NoVerb,
    UnknownVerb(String),
    Arity {
        verb: &'static str,
        usage: &'static str,
    },
    BadTtl(String),
    BadRange(String),
    OddHashPairs(usize),
    Key(KeyError),
    Payload(PayloadError),
}

impl fmt::Display for ParseError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ParseError::NoVerb => write!(f, "no command given"),
            ParseError::UnknownVerb(v) => write!(f, "unknown command {v:?}"),
            ParseError::Arity { verb, usage } => write!(f, "{verb}: usage: {usage}"),
            ParseError::BadTtl(t) => {
                write!(f, "ttl {t:?} is not a positive whole number of seconds")
            }
            ParseError::BadRange(v) => write!(f, "index {v:?} is not a whole number"),
            ParseError::OddHashPairs(n) => {
                write!(
                    f,
                    "hset takes field/value pairs; got {n} trailing arguments"
                )
            }
            ParseError::Key(e) => write!(f, "{e}"),
            ParseError::Payload(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for ParseError {}

impl From<KeyError> for ParseError {
    fn from(e: KeyError) -> Self {
        ParseError::Key(e)
    }
}

impl From<PayloadError> for ParseError {
    fn from(e: PayloadError) -> Self {
        ParseError::Payload(e)
    }
}

/// Every verb and its usage line, so `--help` and the arity errors can never
/// drift apart from what [`parse`] accepts.
pub const USAGE: &[(&str, &str)] = &[
    ("set", "set <key> <json>"),
    ("setex", "setex <key> <ttl-seconds> <json>"),
    ("hset", "hset <key> <field> <value> [<field> <value> …]"),
    ("expire", "expire <key> <ttl-seconds>"),
    ("del", "del <key>"),
    ("lpush", "lpush <key> <json>"),
    ("ltrim", "ltrim <key> <start> <stop>"),
];

fn usage_for(verb: &str) -> &'static str {
    USAGE
        .iter()
        .find(|(name, _)| *name == verb)
        .map(|(_, usage)| *usage)
        .unwrap_or("")
}

fn ttl(text: &str) -> Result<u64, ParseError> {
    match text.parse::<u64>() {
        Ok(n) if n > 0 => Ok(n),
        _ => Err(ParseError::BadTtl(text.to_string())),
    }
}

/// Parse one command from its already-split words.
///
/// `now` is a parameter, never `SystemTime::now()` — the stamping rule is part
/// of what these tests pin.
pub fn parse(words: &[impl AsRef<str>], now: f64) -> Result<Command, ParseError> {
    let words: Vec<&str> = words.iter().map(|w| w.as_ref()).collect();
    let (verb, rest) = words.split_first().ok_or(ParseError::NoVerb)?;
    let verb = *verb;
    let arity = |n: usize| -> Result<(), ParseError> {
        if rest.len() == n {
            Ok(())
        } else {
            Err(ParseError::Arity {
                verb: match verb {
                    "set" => "set",
                    "setex" => "setex",
                    "expire" => "expire",
                    "del" => "del",
                    "lpush" => "lpush",
                    "ltrim" => "ltrim",
                    _ => "hset",
                },
                usage: usage_for(verb),
            })
        }
    };

    match verb {
        "set" => {
            arity(2)?;
            keys::check_key(rest[0])?;
            Ok(Command::SetLatest {
                key: rest[0].into(),
                payload: payload::stamp(rest[1], now)?,
            })
        }
        "setex" => {
            arity(3)?;
            keys::check_key(rest[0])?;
            Ok(Command::SetExpiring {
                key: rest[0].into(),
                ttl: ttl(rest[1])?,
                payload: payload::stamp(rest[2], now)?,
            })
        }
        "hset" => {
            if rest.len() < 3 {
                return Err(ParseError::Arity {
                    verb: "hset",
                    usage: usage_for("hset"),
                });
            }
            if (rest.len() - 1) % 2 != 0 {
                return Err(ParseError::OddHashPairs(rest.len() - 1));
            }
            keys::check_key(rest[0])?;
            let mut pairs: Vec<(String, String)> = rest[1..]
                .chunks(2)
                .map(|pair| (pair[0].to_string(), pair[1].to_string()))
                .collect();
            // rules.md's `ts` rule is about the record, not about its Redis
            // type: a HASH record with no `ts` is one a reader's freshness
            // ladder cannot place. Supplied values always win.
            if !pairs.iter().any(|(field, _)| field == "ts") {
                pairs.push(("ts".into(), format!("{}", now.round() as i64)));
            }
            Ok(Command::HSet {
                key: rest[0].into(),
                pairs,
            })
        }
        "expire" => {
            arity(2)?;
            keys::check_key(rest[0])?;
            Ok(Command::Expire {
                key: rest[0].into(),
                ttl: ttl(rest[1])?,
            })
        }
        "del" => {
            arity(1)?;
            keys::check_key(rest[0])?;
            Ok(Command::Del {
                key: rest[0].into(),
            })
        }
        "lpush" => {
            arity(2)?;
            keys::check_key(rest[0])?;
            Ok(Command::LPush {
                key: rest[0].into(),
                payload: payload::stamp(rest[1], now)?,
            })
        }
        "ltrim" => {
            arity(3)?;
            keys::check_key(rest[0])?;
            let index = |text: &str| {
                text.parse::<i64>()
                    .map_err(|_| ParseError::BadRange(text.to_string()))
            };
            Ok(Command::LTrim {
                key: rest[0].into(),
                start: index(rest[1])?,
                stop: index(rest[2])?,
            })
        }
        other => Err(ParseError::UnknownVerb(other.to_string())),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parsed(words: &[&str]) -> Result<Command, ParseError> {
        parse(words, 1_756_000_000.0)
    }

    #[test]
    fn set_stamps_the_payload_and_takes_no_ttl() {
        let cmd = parsed(&[
            "set",
            "kpidash:services:demo:kai",
            r#"{"state":"ok","text":"up"}"#,
        ])
        .unwrap();
        match cmd {
            Command::SetLatest { key, payload } => {
                assert_eq!(key, "kpidash:services:demo:kai");
                assert!(payload.contains("\"ts\":1756000000.0"), "{payload}");
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn setex_carries_the_ttl() {
        let cmd = parsed(&["setex", "kdash:demo:health", "5", "{}"]).unwrap();
        assert!(matches!(cmd, Command::SetExpiring { ttl: 5, .. }));
    }

    #[test]
    fn hset_gains_a_ts_only_when_the_caller_did_not_send_one() {
        let cmd = parsed(&["hset", "claude:session:kai:abc", "status", "working"]).unwrap();
        match cmd {
            Command::HSet { pairs, .. } => {
                assert_eq!(
                    pairs.last().unwrap(),
                    &("ts".to_string(), "1756000000".to_string())
                );
            }
            other => panic!("{other:?}"),
        }

        let cmd = parsed(&["hset", "claude:session:kai:abc", "ts", "1700000000"]).unwrap();
        match cmd {
            Command::HSet { pairs, .. } => assert_eq!(pairs.len(), 1),
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn bad_arities_name_their_usage_line() {
        assert!(matches!(
            parsed(&["set", "kdash:x"]),
            Err(ParseError::Arity { .. })
        ));
        assert!(matches!(parsed(&["del"]), Err(ParseError::Arity { .. })));
        assert_eq!(
            parsed(&["hset", "claude:limits", "a", "b", "c"]),
            Err(ParseError::OddHashPairs(3))
        );
        assert_eq!(parsed(&[]), Err(ParseError::NoVerb));
        assert_eq!(
            parsed(&["publish", "kdash:x", "{}"]),
            Err(ParseError::UnknownVerb("publish".into()))
        );
    }

    #[test]
    fn every_verb_refuses_an_off_contract_key() {
        // The choke point is the point: no verb is a way around it.
        for words in [
            vec!["set", "weather:now", "{}"],
            vec!["setex", "weather:now", "5", "{}"],
            vec!["hset", "weather:now", "a", "b"],
            vec!["expire", "weather:now", "5"],
            vec!["del", "weather:now"],
            vec!["lpush", "weather:now", "{}"],
            vec!["ltrim", "weather:now", "0", "9"],
        ] {
            assert!(
                matches!(parsed(&words), Err(ParseError::Key(_))),
                "{words:?} should have been refused"
            );
        }
    }

    #[test]
    fn a_zero_ttl_is_refused_rather_than_written() {
        // SET … EX 0 is an error in Redis, and EXPIRE 0 deletes the key —
        // neither is what a publisher meant by "no expiry".
        assert!(matches!(
            parsed(&["setex", "kdash:x:y", "0", "{}"]),
            Err(ParseError::BadTtl(_))
        ));
        assert!(matches!(
            parsed(&["expire", "kdash:x:y", "-1"]),
            Err(ParseError::BadTtl(_))
        ));
    }

    #[test]
    fn a_malformed_payload_never_reaches_redis() {
        assert!(matches!(
            parsed(&["set", "kdash:x:y", "[]"]),
            Err(ParseError::Payload(_))
        ));
        assert!(matches!(
            parsed(&["lpush", "kdash:x:y", "nope"]),
            Err(ParseError::Payload(_))
        ));
    }

    #[test]
    fn usage_covers_exactly_the_verbs_parse_accepts() {
        for (verb, usage) in USAGE {
            assert!(!usage.is_empty(), "{verb} has no usage line");
            assert!(
                !matches!(parsed(&[verb]), Err(ParseError::UnknownVerb(_))),
                "{verb} is documented but not accepted"
            );
        }
    }
}
