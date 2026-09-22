//! `kdash-pub` — the fast-startup publisher CLI.
//!
//! This exists because the publishers that matter most are **shell** — Claude
//! Code hooks and statuslines, which fire on every prompt and every tool call
//! and must not be felt. They cannot import a Python package at that cadence,
//! and hand-rolling RESP over `/dev/tcp` (what `claude-pub.sh` does today) buys
//! speed by opting out of every contract this repo owns: no khlenv, no AUTH,
//! no key grammar, a hardcoded IP.
//!
//! So: an exec of a small native binary that has all of them. This is the
//! CD-7 cutover vehicle.
//!
//! ```sh
//! kdash-pub set kpidash:services:demo:kai '{"state":"ok","text":"up"}'
//! kdash-pub setex kdash:demo:health 5 '{"alive":true}'
//! kdash-pub --stem KDASH_CLAUDE_REDIS hset claude:session:kai:abc status working
//! kdash-pub endpoint                                  # where would this write?
//! kdash-pub check                                     # ...and would it be accepted?
//! kdash-pub --stem KDASH_CLAUDE_REDIS hget claude:limits updated_at
//! kdash-pub get kdash:stale:komarchy:claude-hooks      # 0 value / 1 absent / 2 unknown
//! kdash-pub scan 'kdash:stale:komarchy:*'              # one key per line
//! printf 'hset\tclaude:session:kai:abc\tstatus\tworking\nexpire\tclaude:session:kai:abc\t7200\n' \
//!   | kdash-pub --best-effort batch
//! ```
//!
//! Exit codes are the interesting part for a hook:
//!
//! | code | means |
//! |---|---|
//! | 0 | published (or `--best-effort` swallowed a delivery failure) |
//! | 1 | the command is wrong — bad key, bad payload, bad usage |
//! | 2 | delivery failed — no endpoint, no auth, Redis unreachable |
//!
//! `--best-effort` turns 2 into 0 and never touches 1. A dead Redis must not
//! fail a hook; a key that violates the grammar is a bug that should be
//! noticed, and quietly exiting 0 on it is how a publisher goes off-contract
//! for a month without anyone finding out.
//!
//! `hget` (CD-14) shares those codes exactly, which is what lets a caller keep
//! one error convention across its reads and its writes: **0 with the value on
//! stdout, 0 with nothing on stdout when the field is absent** — a missing
//! field is an answer, not a fault — and 2 when Redis could not be reached, so
//! `--best-effort` degrades a read to "unknown" the same way it degrades a
//! write to "dropped".
//!
//! `get` and `scan` (sprint 015) do **not** share them, and the reason is the
//! whole point of the two verbs:
//!
//! | code | `get <key>` | `scan <pattern>` |
//! |---|---|---|
//! | 0 | present — value on stdout | answered — one key per line, possibly none |
//! | 1 | **absent** — nothing on stdout | (not used) |
//! | 2 | could not ask: Redis unreachable, auth failed, *or the command was refused* | same |
//!
//! `hget` has no code meaning "absent", so 1 is free there for a bad command.
//! `get` needs 1 for absent, so a refused command moves to 2 — where it is
//! true, because no answer came back either way. `scan` follows `get` so the
//! two new verbs read alike; its empty answer is exit 0, because "no keys
//! matched" is complete rather than missing.
//!
//! `check` (sprint 016) is the third verb on that pattern, and it exists
//! because **`endpoint` answers a narrower question than it looks like it
//! answers**:
//!
//! > `endpoint` resolves the endpoint and opens a socket. It issues **no
//! > command**. Redis checks AUTH when AUTH is *sent*, so a wrong password
//! > fails right here — but `--no-auth` against a server that requires one
//! > connects happily and fails `NOAUTH` on the first real command. So
//! > `kdash-pub --no-auth endpoint` exits **0** against the authenticated
//! > central Redis, for a configuration that cannot write a single key.
//! > Measured on kubs0 2026-09-12 and again on kai 2026-09-21 (WI 2492).
//!
//! That is not a bug in `endpoint` — "where would this write" is a fair
//! question with a fair answer, and it is the cheap one a caller wants when
//! that is all it is asking. `check` is the other question, and it round-trips
//! a `PING` so exit 0 means the server accepted an authenticated command:
//!
//! | code | `check` |
//! |---|---|
//! | 0 | resolved, connected, and a command came back |
//! | 1 | **deliberately nowhere** — khlenv holds an explicit null for this stem |
//! | 2 | could not: unreachable, auth failed, or the command was refused |
//!
//! Exit 1 is the same shape `get` uses: a clean negative answer that is not a
//! fault. A host khlenv says publishes nowhere is *correctly configured*, and
//! reporting it as an unreachable Redis would be the same conflation this
//! whole table exists to prevent.
//!
//! **`--best-effort` does not touch `get`, `scan` or `check`.** It exists so a
//! dead Redis cannot fail a hook, and turning a 2 into a 0 here would make "I could
//! not ask" indistinguishable from "present and empty" on `get`, or from "no
//! keys" on `scan`. These verbs are read-modify-write guards whose entire job
//! is refusing to guess — `kdash:stale`'s `since` must be carried forward
//! unchanged across every later skip (CD-18), and a caller that read an
//! unreachable Redis as "absent" would restamp it, turning "stale for three
//! weeks" into "stale for an hour" while looking like working code.

use kdash_pub::endpoint::{self, Resolved, Stem};
use kdash_pub::{command, Answer, Command, Publisher, Query};
use std::io::{Read, Write};
use std::process::ExitCode;

const EXIT_USAGE: u8 = 1;
const EXIT_DELIVERY: u8 = 2;
/// `get`'s "the key is not there" — a clean answer, not a fault. Numerically
/// [`EXIT_USAGE`], which is why `get` reports a refused command as
/// [`EXIT_DELIVERY`] instead: one code cannot mean both "no" and "you asked
/// wrong" on a verb whose exit status IS the answer.
const EXIT_ABSENT: u8 = 1;
/// `check`'s "khlenv holds an explicit null for this stem" — this host is
/// configured to publish nowhere, which is an answer and not a fault. Same
/// numeric as [`EXIT_ABSENT`] and for the same reason: on a verb whose exit
/// status IS the answer, 1 is the clean negative and 2 is "could not ask".
const EXIT_NOWHERE: u8 = 1;

/// Everything the argv front end decides, separated from doing any of it.
#[derive(Debug, Clone, PartialEq, Eq)]
struct Invocation {
    app: String,
    stem: Stem,
    pinned: Option<String>,
    no_auth: bool,
    best_effort: bool,
    verbose: bool,
    action: Action,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum Action {
    /// One command from argv.
    Publish(Vec<String>),
    /// Tab-separated commands from stdin, all in one round trip.
    Batch,
    /// One read from argv (CD-14): `hget`, `get` or `scan`.
    Read(Vec<String>),
    /// Print where this invocation would write, and connect to prove it.
    /// Connecting is NOT proof that a write would land — see `Check`.
    Endpoint,
    /// Everything `Endpoint` does, plus a command round trip, so exit 0 means
    /// the server accepted an authenticated command (WI 2492).
    Check,
    Help,
    Version,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ArgError {
    NeedsValue(String),
    Unknown(String),
    NoCommand,
}

impl std::fmt::Display for ArgError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            ArgError::NeedsValue(flag) => write!(f, "{flag} needs a value"),
            ArgError::Unknown(flag) => write!(f, "unknown option {flag}"),
            ArgError::NoCommand => write!(f, "no command given"),
        }
    }
}

/// Parse argv.
///
/// Hand-rolled rather than `clap`: this binary is exec'd on every tool call in
/// every Claude session on the box, so its dependency list is a latency
/// decision, not a convenience one. Seven verbs and five flags do not need an
/// argument framework.
fn parse_args(argv: &[String]) -> Result<Invocation, ArgError> {
    let mut invocation = Invocation {
        app: "kdash-pub".to_string(),
        stem: Stem::CENTRAL,
        pinned: None,
        no_auth: false,
        best_effort: false,
        verbose: false,
        action: Action::Help,
    };

    let mut it = argv.iter();
    let mut rest: Vec<String> = Vec::new();
    while let Some(arg) = it.next() {
        let mut value = |flag: &str| {
            it.next()
                .cloned()
                .ok_or_else(|| ArgError::NeedsValue(flag.to_string()))
        };
        match arg.as_str() {
            "--app" => invocation.app = value("--app")?,
            "--stem" => invocation.stem = Stem::named(&value("--stem")?),
            "--endpoint" => invocation.pinned = Some(value("--endpoint")?),
            "--no-auth" => invocation.no_auth = true,
            "--best-effort" => invocation.best_effort = true,
            "--verbose" | "-v" => invocation.verbose = true,
            "--help" | "-h" => {
                return Ok(Invocation {
                    action: Action::Help,
                    ..invocation
                })
            }
            "--version" | "-V" => {
                return Ok(Invocation {
                    action: Action::Version,
                    ..invocation
                })
            }
            // A payload can start with `-`; once the verb is seen, nothing
            // further is a flag.
            other if other.starts_with("--") && rest.is_empty() => {
                return Err(ArgError::Unknown(other.to_string()))
            }
            other => {
                rest.push(other.to_string());
                rest.extend(it.by_ref().cloned());
                break;
            }
        }
    }

    invocation.action = match rest.first().map(String::as_str) {
        None => return Err(ArgError::NoCommand),
        Some("batch") => Action::Batch,
        Some("endpoint") => Action::Endpoint,
        Some("check") => Action::Check,
        Some("help") => Action::Help,
        // The table decides, not a literal here: a second read verb should
        // reach the read path by being added to READ_USAGE and nowhere else.
        Some(verb) if command::is_read_verb(verb) => Action::Read(rest),
        Some(_) => Action::Publish(rest),
    };
    Ok(invocation)
}

/// Split one batch line into words.
///
/// TAB, because the payloads are JSON: a JSON string cannot contain a literal
/// tab (it must be `\t`), so this needs no quoting rules and no escaping — and
/// a format with no escaping has no escaping bugs. Blank lines and `#`
/// comments are skipped so a generated batch stays readable.
fn batch_line(line: &str) -> Option<Vec<String>> {
    let trimmed = line.trim_end_matches(['\r', '\n']);
    if trimmed.trim().is_empty() || trimmed.trim_start().starts_with('#') {
        return None;
    }
    Some(trimmed.split('\t').map(str::to_string).collect())
}

fn help() -> String {
    let verbs = command::USAGE
        .iter()
        .chain(command::READ_USAGE)
        .map(|(_, usage)| format!("  kdash-pub [options] {usage}"))
        .collect::<Vec<_>>()
        .join("\n");
    format!(
        "kdash-pub — publish to the kdashdata Redis feeds (contracts/rules.md)\n\
         \n\
         Usage:\n\
         {verbs}\n\
         \x20 kdash-pub [options] batch          # tab-separated commands on stdin, one round trip\n\
         \x20 kdash-pub [options] endpoint       # print where this would write, and connect\n\
         \x20 kdash-pub [options] check          # ...and round-trip a command to prove it\n\
         \n\
         Options:\n\
         \x20 --app <name>        app name sent to khlenv (default: kdash-pub)\n\
         \x20 --stem <KEY>        stem to resolve (default: {central}; {claude} for the claude feed)\n\
         \x20 --endpoint <h:p>    pin the endpoint, skipping khlenv entirely\n\
         \x20 --no-auth           send no AUTH — for a Redis with no password configured\n\
         \x20                     (the interim claude home takes none; central requires one)\n\
         \x20 --best-effort       exit 0 when delivery fails; contract errors still exit 1\n\
         \x20 --verbose, -v       print the endpoint written to\n\
         \x20 --help, --version\n\
         \n\
         The password comes from $REDISCLI_AUTH, or from an env file when it is\n\
         unset (CD-12, CD-19), in order: $KDASH_AUTH_FILE (exclusive), then the\n\
         per-host file -- %ProgramData%\\khomelab\\secrets.env where ProgramData\n\
         is set, else /etc/khomelab/secrets.env -- then the deprecated per-user\n\
         files ~/.config/kdash/redis-auth.env and\n\
         ~/.config/kpidash-client/redis-auth.env.\n\
         \n\
         hget prints the value and a newline, or nothing at all when the field\n\
         is absent — an absent field is an answer, not an error.\n\
         \n\
         get prints the value and a newline and exits 0, or exits 1 with nothing\n\
         printed when the key is absent; scan prints one matching key per line\n\
         and exits 0 even when none matched. Both exit 2 when the question could\n\
         not be asked at all — an unreachable Redis or a refused key — and\n\
         --best-effort does NOT turn that into a 0, because for these two the\n\
         exit code is the answer.\n\
         \n\
         endpoint answers WHERE this host would write. It issues no command, so\n\
         it cannot answer whether the write would be accepted: --no-auth exits 0\n\
         against a Redis that requires a password, because Redis only checks\n\
         AUTH when AUTH is sent. Use check for that — it round-trips a PING and\n\
         exits 0 accepted / 1 khlenv says nowhere / 2 could not ask, with\n\
         --best-effort leaving all three alone.\n",
        central = endpoint::CENTRAL_STEM,
        claude = endpoint::CLAUDE_STEM,
    )
}

fn build_commands(action: &Action) -> Result<Vec<Command>, String> {
    let now = kdash_pub::now();
    match action {
        Action::Publish(words) => Ok(vec![command::parse(words, now).map_err(|e| e.to_string())?]),
        Action::Batch => {
            let mut text = String::new();
            std::io::stdin()
                .read_to_string(&mut text)
                .map_err(|e| format!("reading stdin: {e}"))?;
            let mut commands = Vec::new();
            for (n, line) in text.lines().enumerate() {
                let Some(words) = batch_line(line) else {
                    continue;
                };
                commands
                    .push(command::parse(&words, now).map_err(|e| format!("line {}: {e}", n + 1))?);
            }
            Ok(commands)
        }
        _ => Ok(Vec::new()),
    }
}

/// The read half of [`build_commands`]. Both run before anything connects, so
/// a malformed read is reported as a usage error even with Redis down.
fn build_query(action: &Action) -> Result<Option<Query>, String> {
    match action {
        Action::Read(words) => Ok(Some(
            command::parse_query(words).map_err(|e| e.to_string())?,
        )),
        _ => Ok(None),
    }
}

fn publisher(invocation: &Invocation) -> Result<Publisher, String> {
    let mut publisher = Publisher::new(&invocation.app, invocation.stem.clone());
    if invocation.no_auth {
        publisher = publisher.without_auth();
    }
    if let Some(pinned) = &invocation.pinned {
        let (host, port) = endpoint::parse_hostport(pinned, endpoint::REDIS_PORT_DEFAULT)
            .map_err(|e| e.to_string())?;
        publisher = publisher.with_endpoint(host, port);
    }
    Ok(publisher)
}

/// True for the verbs whose EXIT CODE is the answer — `get` and `scan`.
///
/// Two things hang off this and both are the same rule: a refused command
/// reports [`EXIT_DELIVERY`] rather than [`EXIT_USAGE`] (because 1 already
/// means "absent"), and `--best-effort` leaves that 2 alone (because turning
/// it into 0 would spell "I could not ask" the same as "here is the answer").
fn answers_with_status(action: &Action) -> bool {
    match action {
        // `check` is a probe whose whole output is its exit code.
        Action::Check => true,
        Action::Read(words) => words.first().is_some_and(|v| v == "get" || v == "scan"),
        _ => false,
    }
}

fn run(invocation: Invocation) -> Result<(), (u8, String)> {
    // On `get`/`scan` a refused command is a could-not-ask, not a usage error:
    // exit 1 is taken by "absent", and a caller branching on it must never read
    // "your key was rejected" as "the key is not set".
    let refused = if answers_with_status(&invocation.action) {
        EXIT_DELIVERY
    } else {
        EXIT_USAGE
    };
    // Everything that can be decided without a socket is decided first, so a
    // contract error is reported as one even when Redis is down.
    let commands = build_commands(&invocation.action).map_err(|e| (EXIT_USAGE, e))?;
    let query = build_query(&invocation.action).map_err(|e| (refused, e))?;
    let publisher = publisher(&invocation).map_err(|e| (refused, e))?;

    // `endpoint` and `check` ask the same question one clause apart, so they
    // share everything up to the round trip.
    let probing = matches!(invocation.action, Action::Endpoint | Action::Check);
    if probing {
        match publisher
            .resolve()
            .map_err(|e| (EXIT_DELIVERY, e.to_string()))?
        {
            Resolved::At { host, port } => println!("{host}:{port}"),
            Resolved::Nowhere => {
                println!("(none)");
                // For `endpoint` this IS the answer: nowhere, deliberately.
                // For `check` it is a clean negative — this host is configured
                // not to publish — and it gets its own code rather than being
                // folded into the unreachable-Redis 2, which would say
                // something false about the world.
                return if matches!(invocation.action, Action::Check) {
                    Err((
                        EXIT_NOWHERE,
                        format!(
                            "khlenv holds an explicit null for {} — this host \
                             deliberately publishes nowhere",
                            invocation.stem.key
                        ),
                    ))
                } else {
                    Ok(())
                };
            }
        }
    }

    let mut connection = publisher
        .connect()
        .map_err(|e| (EXIT_DELIVERY, e.to_string()))?;
    if invocation.verbose || probing {
        eprintln!("kdash-pub: {}", connection.endpoint());
        // `endpoint` exists to answer "can this host publish, and how" — which
        // file answered is half of that, and it is the one half no amount of
        // staring at the output could otherwise recover (CD-19).
        eprintln!("kdash-pub: auth from {}", connection.auth_origin());
    }
    // Always, not only under --verbose: the per-user files are on their way out
    // with the k-homelab changeover, and a host still answering from one is the
    // thing that needs to become visible before they are deleted (CD-19).
    if connection
        .auth_source()
        .is_some_and(|source| source.deprecated())
    {
        eprintln!(
            "kdash-pub: warning: the password came from {} — deprecated, \
             superseded by the per-host secrets file (CD-19)",
            connection
                .auth_path()
                .map(|p| p.display().to_string())
                .unwrap_or_else(|| "a per-user env file".to_string())
        );
    }
    if matches!(invocation.action, Action::Check) {
        connection
            .check()
            .map_err(|e| (EXIT_DELIVERY, e.to_string()))?;
        // On stderr, like the other two probe lines: stdout is `host:port` and
        // stays parseable by whatever already pipes `endpoint`.
        eprintln!("kdash-pub: PING answered — this endpoint accepts authenticated commands");
        return Ok(());
    }

    if let Some(query) = &query {
        let answer = connection
            .read(query)
            .map_err(|e| (EXIT_DELIVERY, e.to_string()))?;
        let mut out = std::io::stdout().lock();
        match answer {
            Answer::Value(Some(value)) => out
                .write_all(&value)
                .and_then(|()| out.write_all(b"\n"))
                .map_err(|e| (EXIT_DELIVERY, format!("writing stdout: {e}")))?,
            // `hget`: nothing printed, exit 0 — the caller's empty read is the
            // "unknown" its guard is written to expect (CD-14).
            Answer::Value(None) if matches!(query, Query::HGet { .. }) => {}
            // `get`: absence is its own answer and must not be confused with a
            // present-but-empty value, which is what exit 0 with no output
            // would say.
            Answer::Value(None) => {
                return Err((EXIT_ABSENT, format!("{} is not set", query.key())))
            }
            Answer::Keys(keys) => {
                for key in keys {
                    writeln!(out, "{key}")
                        .map_err(|e| (EXIT_DELIVERY, format!("writing stdout: {e}")))?;
                }
            }
        }
        return Ok(());
    }

    connection
        .pipeline(&commands)
        .map_err(|e| (EXIT_DELIVERY, e.to_string()))
}

fn main() -> ExitCode {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let invocation = match parse_args(&argv) {
        Ok(invocation) => invocation,
        Err(error) => {
            eprintln!("kdash-pub: {error}\n\n{}", help());
            return ExitCode::from(EXIT_USAGE);
        }
    };

    match invocation.action {
        Action::Help => {
            print!("{}", help());
            return ExitCode::SUCCESS;
        }
        Action::Version => {
            println!("kdash-pub {}", env!("KDASH_PUB_VERSION_FULL"));
            return ExitCode::SUCCESS;
        }
        _ => {}
    }

    // `--best-effort` is for the hook path: a dead Redis must not fail a hook.
    // It cannot apply to a verb whose exit code IS the answer — see the table
    // at the top of this file.
    let best_effort = invocation.best_effort && !answers_with_status(&invocation.action);
    match run(invocation) {
        Ok(()) => ExitCode::SUCCESS,
        Err((code, message)) => {
            eprintln!("kdash-pub: {message}");
            if best_effort && code == EXIT_DELIVERY {
                ExitCode::SUCCESS
            } else {
                ExitCode::from(code)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn args(words: &[&str]) -> Result<Invocation, ArgError> {
        parse_args(&words.iter().map(|w| w.to_string()).collect::<Vec<_>>())
    }

    #[test]
    fn defaults_are_the_central_stem_and_no_pin() {
        let invocation = args(&["set", "kdash:x:y", "{}"]).unwrap();
        assert_eq!(invocation.stem, Stem::CENTRAL);
        assert_eq!(invocation.app, "kdash-pub");
        assert!(invocation.pinned.is_none());
        assert!(!invocation.best_effort);
        assert_eq!(
            invocation.action,
            Action::Publish(vec!["set".into(), "kdash:x:y".into(), "{}".into()])
        );
    }

    #[test]
    fn the_claude_stem_keeps_its_no_default_walk_when_named_on_the_cli() {
        let invocation = args(&["--stem", "KDASH_CLAUDE_REDIS", "del", "claude:limits"]).unwrap();
        assert_eq!(invocation.stem, Stem::CLAUDE);
        assert!(invocation.stem.default_value.is_none());
    }

    #[test]
    fn an_unknown_stem_gets_no_alias_and_no_default() {
        let invocation = args(&["--stem", "SOMETHING_ELSE", "del", "kdash:x"]).unwrap();
        assert_eq!(invocation.stem.key, "SOMETHING_ELSE");
        assert!(invocation.stem.legacy.is_none());
        assert!(invocation.stem.default_value.is_none());
    }

    #[test]
    fn flags_stop_at_the_verb_so_a_payload_may_start_with_a_dash() {
        // `{"delta":-1}` is fine; a bare `--foo` payload would be too.
        let invocation = args(&["set", "kdash:x:y", "--not-a-flag"]).unwrap();
        assert_eq!(
            invocation.action,
            Action::Publish(vec![
                "set".into(),
                "kdash:x:y".into(),
                "--not-a-flag".into()
            ])
        );
    }

    #[test]
    fn no_auth_is_off_unless_asked_for() {
        assert!(!args(&["del", "kdash:x"]).unwrap().no_auth);
        assert!(
            args(&["--no-auth", "del", "claude:limits"])
                .unwrap()
                .no_auth
        );
    }

    #[test]
    fn a_typo_before_the_verb_is_still_caught() {
        assert_eq!(
            args(&["--best-efort", "del", "kdash:x"]),
            Err(ArgError::Unknown("--best-efort".into()))
        );
        assert_eq!(
            args(&["--stem"]),
            Err(ArgError::NeedsValue("--stem".into()))
        );
        assert_eq!(args(&[]), Err(ArgError::NoCommand));
    }

    #[test]
    fn hget_reaches_the_read_path_and_never_the_publish_one() {
        let invocation = args(&[
            "--stem",
            "KDASH_CLAUDE_REDIS",
            "hget",
            "claude:limits",
            "updated_at",
        ])
        .unwrap();
        assert_eq!(
            invocation.action,
            Action::Read(vec![
                "hget".into(),
                "claude:limits".into(),
                "updated_at".into()
            ])
        );
        assert_eq!(invocation.stem, Stem::CLAUDE);
        // A read builds no commands and a write builds no query — the two
        // paths never both fire for one invocation.
        assert!(build_commands(&invocation.action).unwrap().is_empty());
        assert_eq!(
            build_query(&invocation.action).unwrap(),
            Some(Query::HGet {
                key: "claude:limits".into(),
                field: "updated_at".into()
            })
        );
        assert!(
            build_query(&args(&["del", "claude:limits"]).unwrap().action)
                .unwrap()
                .is_none()
        );
    }

    #[test]
    fn the_new_read_verbs_reach_the_read_path() {
        for (words, expected) in [
            (
                vec!["get", "kdash:stale:komarchy:claude-hooks"],
                Query::Get {
                    key: "kdash:stale:komarchy:claude-hooks".into(),
                },
            ),
            (
                vec!["scan", "kdash:stale:komarchy:*"],
                Query::Scan {
                    pattern: "kdash:stale:komarchy:*".into(),
                },
            ),
        ] {
            let invocation = args(&words).unwrap();
            assert_eq!(
                invocation.action,
                Action::Read(words.iter().map(|w| w.to_string()).collect())
            );
            assert!(build_commands(&invocation.action).unwrap().is_empty());
            assert_eq!(build_query(&invocation.action).unwrap(), Some(expected));
        }
    }

    #[test]
    fn only_the_verbs_whose_exit_code_is_an_answer_opt_out_of_best_effort() {
        // The distinction this whole exit-code table exists for: `hget` says
        // "absent" with exit 0, so --best-effort may safely fold a delivery
        // failure into the same answer. `get` says it with exit 1, so folding
        // would make "could not ask" read as "here is the value" — and for
        // kdash:stale that restamps a `since` the caller was told to carry
        // forward unchanged.
        assert!(answers_with_status(
            &args(&["get", "claude:limits"]).unwrap().action
        ));
        assert!(answers_with_status(
            &args(&["scan", "kdash:stale:*:*"]).unwrap().action
        ));
        assert!(!answers_with_status(
            &args(&["hget", "claude:limits", "updated_at"])
                .unwrap()
                .action
        ));
        assert!(!answers_with_status(
            &args(&["del", "kdash:x:y"]).unwrap().action
        ));
        assert!(!answers_with_status(&args(&["batch"]).unwrap().action));
    }

    #[test]
    fn batch_and_endpoint_are_actions_not_keys() {
        assert_eq!(args(&["batch"]).unwrap().action, Action::Batch);
        assert_eq!(args(&["endpoint"]).unwrap().action, Action::Endpoint);
        assert_eq!(args(&["check"]).unwrap().action, Action::Check);
        assert_eq!(args(&["--help"]).unwrap().action, Action::Help);
        assert_eq!(args(&["-V"]).unwrap().action, Action::Version);
    }

    #[test]
    fn check_is_a_probe_and_reaches_neither_the_write_nor_the_read_path() {
        // It takes no key and no payload, so the two argv-to-work builders
        // must both come back empty — otherwise `check` would be parsed as a
        // write to a key called "check".
        let invocation = args(&["--app", "kdashdata", "check"]).unwrap();
        assert_eq!(invocation.action, Action::Check);
        assert_eq!(invocation.app, "kdashdata");
        assert!(build_commands(&invocation.action).unwrap().is_empty());
        assert!(build_query(&invocation.action).unwrap().is_none());
        // And it carries the flags a probe needs: a pinned endpoint and a
        // named stem are the two ways an operator asks about somewhere else.
        let pinned = args(&["--stem", "KDASH_CLAUDE_REDIS", "--no-auth", "check"]).unwrap();
        assert_eq!(pinned.stem, Stem::CLAUDE);
        assert!(pinned.no_auth);
        assert_eq!(pinned.action, Action::Check);
    }

    #[test]
    fn check_opts_out_of_best_effort_like_the_other_answering_verbs() {
        // The reason this matters: `check` exists to say whether a write would
        // land. --best-effort folding its 2 into a 0 would make the one verb
        // that answers that question answer it wrong, on exactly the hosts
        // (hook paths) that pass --best-effort by habit.
        assert!(answers_with_status(&args(&["check"]).unwrap().action));
        // `endpoint` is NOT on this list, and deliberately: it answers where,
        // not whether, and has done since sprint 003.
        assert!(!answers_with_status(&args(&["endpoint"]).unwrap().action));
    }

    #[test]
    fn help_names_check_and_the_endpoint_trap_it_exists_for() {
        let text = help();
        assert!(
            text.contains("kdash-pub [options] check"),
            "help omits check"
        );
        // The trap has to be visible where somebody meets it (WI 2492): a
        // reader of --help must not have to already know that `endpoint`
        // issues no command.
        assert!(
            text.contains("issues no command"),
            "help omits the endpoint caveat"
        );
        assert!(
            text.contains("--no-auth exits 0"),
            "help omits the measured case"
        );
    }

    #[test]
    fn batch_lines_split_on_tabs_and_skip_noise() {
        assert_eq!(
            batch_line("hset\tclaude:session:kai:abc\tstatus\tworking"),
            Some(vec![
                "hset".into(),
                "claude:session:kai:abc".into(),
                "status".into(),
                "working".into()
            ])
        );
        // A JSON payload with spaces survives; only tabs split.
        assert_eq!(
            batch_line("set\tkdash:x:y\t{\"text\": \"two words\"}\r\n"),
            Some(vec![
                "set".into(),
                "kdash:x:y".into(),
                "{\"text\": \"two words\"}".into()
            ])
        );
        assert_eq!(batch_line(""), None);
        assert_eq!(batch_line("   "), None);
        assert_eq!(batch_line("# a comment"), None);
    }

    #[test]
    fn help_lists_every_verb_the_parser_accepts() {
        let text = help();
        for (verb, _) in command::USAGE.iter().chain(command::READ_USAGE) {
            assert!(text.contains(&format!(" {verb} ")), "help omits {verb}");
        }
        assert!(text.contains(endpoint::CLAUDE_STEM));
    }
}
