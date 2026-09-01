//! The verbs, parsed and validated before anything opens a socket.
//!
//! Almost all of them are writes. [`Query`] is the one read (CD-14), and it
//! lives here for the same reason the writes do: it goes through the same key
//! grammar, so there is one place a key is judged rather than two.
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
    /// A read verb where a write was expected — a `batch` line, typically.
    /// Distinguished from [`ParseError::UnknownVerb`] because "unknown
    /// command" sends you looking for a typo that is not there, and one bad
    /// line takes the whole batch down with it.
    ReadVerbInWriteContext(&'static str),
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
            ParseError::ReadVerbInWriteContext(v) => write!(
                f,
                "{v} reads, so it cannot appear where a write is expected \
                 (run it on its own: kdash-pub {})",
                usage_for(v)
            ),
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

/// The read verbs. Deliberately a second table rather than a row in [`USAGE`]:
/// everything there is a write that [`parse`] turns into a [`Command`] and
/// `pipeline` runs with its reply ignored, and a read is neither of those.
/// `--help` renders both.
pub const READ_USAGE: &[(&str, &str)] = &[("hget", "hget <key> <field>")];

/// True for a verb [`parse_query`] accepts.
pub fn is_read_verb(verb: &str) -> bool {
    READ_USAGE.iter().any(|(name, _)| *name == verb)
}

fn usage_for(verb: &str) -> &'static str {
    USAGE
        .iter()
        .chain(READ_USAGE)
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
        other if is_read_verb(other) => Err(ParseError::ReadVerbInWriteContext(
            READ_USAGE
                .iter()
                .find(|(name, _)| *name == other)
                .map(|(name, _)| *name)
                .unwrap_or("that verb"),
        )),
        other => Err(ParseError::UnknownVerb(other.to_string())),
    }
}

/// One validated read.
///
/// A publisher reads for exactly one reason: to guard its own write against
/// clobbering a fresher observation (CD-14). This is not the consumer side —
/// there is no data model here and no freshness policy, just a field.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Query {
    /// One field of a HASH record. An absent field and an absent key are the
    /// same answer — nothing — because Redis does not distinguish them and
    /// neither does the guard this serves.
    HGet { key: String, field: String },
}

impl Query {
    pub fn key(&self) -> &str {
        match self {
            Query::HGet { key, .. } => key,
        }
    }
}

/// Parse one read from its already-split words.
///
/// No `now` parameter, unlike [`parse`]: a read stamps nothing, so there is no
/// clock to inject and nothing about it that a test would need to pin.
pub fn parse_query(words: &[impl AsRef<str>]) -> Result<Query, ParseError> {
    let words: Vec<&str> = words.iter().map(|w| w.as_ref()).collect();
    let (verb, rest) = words.split_first().ok_or(ParseError::NoVerb)?;
    match *verb {
        "hget" => {
            if rest.len() != 2 {
                return Err(ParseError::Arity {
                    verb: "hget",
                    usage: usage_for("hget"),
                });
            }
            // The same choke point the writes go through: a key no reader will
            // parse is not one a publisher may ask about either.
            keys::check_key(rest[0])?;
            Ok(Query::HGet {
                key: rest[0].into(),
                field: rest[1].into(),
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
    fn hget_parses_a_key_and_a_field() {
        assert_eq!(
            parse_query(&["hget", "claude:limits", "updated_at"]),
            Ok(Query::HGet {
                key: "claude:limits".into(),
                field: "updated_at".into(),
            })
        );
    }

    #[test]
    fn a_read_goes_through_the_same_key_grammar_as_a_write() {
        // The choke point is not something a read gets to walk around.
        assert!(matches!(
            parse_query(&["hget", "weather:now", "temp"]),
            Err(ParseError::Key(_))
        ));
    }

    #[test]
    fn hget_names_its_usage_line_on_a_bad_arity() {
        for words in [
            vec!["hget"],
            vec!["hget", "claude:limits"],
            vec!["hget", "claude:limits", "updated_at", "extra"],
        ] {
            assert!(
                matches!(
                    parse_query(&words),
                    Err(ParseError::Arity { verb: "hget", .. })
                ),
                "{words:?} should have been an arity error"
            );
        }
        let no_words: [&str; 0] = [];
        assert_eq!(parse_query(&no_words), Err(ParseError::NoVerb));
    }

    #[test]
    fn a_write_verb_is_not_a_read_and_a_read_verb_is_not_a_write() {
        assert_eq!(
            parse_query(&["hset", "claude:limits", "a", "b"]),
            Err(ParseError::UnknownVerb("hset".into()))
        );
        // In a batch this is the difference between "look for your typo" and
        // "this line reads" — and one bad line refuses the whole batch, so the
        // message is the only clue the caller gets.
        assert_eq!(
            parsed(&["hget", "claude:limits", "updated_at"]),
            Err(ParseError::ReadVerbInWriteContext("hget"))
        );
        assert!(parsed(&["hget", "claude:limits", "updated_at"])
            .unwrap_err()
            .to_string()
            .contains("hget <key> <field>"));
    }

    #[test]
    fn read_usage_covers_exactly_the_verbs_parse_query_accepts() {
        for (verb, usage) in READ_USAGE {
            assert!(!usage.is_empty(), "{verb} has no usage line");
            assert!(
                is_read_verb(verb),
                "{verb} is documented but not a read verb"
            );
            assert!(
                !matches!(parse_query(&[verb]), Err(ParseError::UnknownVerb(_))),
                "{verb} is documented but not accepted"
            );
        }
        // And no verb is in both tables — that would make `parse` and
        // `parse_query` disagree about what the word means.
        for (write_verb, _) in USAGE {
            assert!(!is_read_verb(write_verb), "{write_verb} is in both tables");
        }
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
