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
//! kdash-pub endpoint
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

use kdash_pub::endpoint::{self, Resolved, Stem};
use kdash_pub::{command, Command, Publisher};
use std::io::Read;
use std::process::ExitCode;

const EXIT_USAGE: u8 = 1;
const EXIT_DELIVERY: u8 = 2;

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
    /// Print where this invocation would write, and connect to prove it.
    Endpoint,
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
        Some("help") => Action::Help,
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
         The password comes from $REDISCLI_AUTH, or from a 0600 env file when it\n\
         is unset (CD-12): $KDASH_AUTH_FILE, ~/.config/kdash/redis-auth.env,\n\
         ~/.config/kpidash-client/redis-auth.env.\n",
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

fn run(invocation: Invocation) -> Result<(), (u8, String)> {
    // Everything that can be decided without a socket is decided first, so a
    // contract error is reported as one even when Redis is down.
    let commands = build_commands(&invocation.action).map_err(|e| (EXIT_USAGE, e))?;
    let publisher = publisher(&invocation).map_err(|e| (EXIT_USAGE, e))?;

    if matches!(invocation.action, Action::Endpoint) {
        match publisher
            .resolve()
            .map_err(|e| (EXIT_DELIVERY, e.to_string()))?
        {
            Resolved::At { host, port } => println!("{host}:{port}"),
            Resolved::Nowhere => {
                println!("(none)");
                return Ok(());
            }
        }
    }

    let mut connection = publisher
        .connect()
        .map_err(|e| (EXIT_DELIVERY, e.to_string()))?;
    if invocation.verbose || matches!(invocation.action, Action::Endpoint) {
        eprintln!("kdash-pub: {}", connection.endpoint());
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
            println!("kdash-pub {}", env!("CARGO_PKG_VERSION"));
            return ExitCode::SUCCESS;
        }
        _ => {}
    }

    let best_effort = invocation.best_effort;
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
    fn batch_and_endpoint_are_actions_not_keys() {
        assert_eq!(args(&["batch"]).unwrap().action, Action::Batch);
        assert_eq!(args(&["endpoint"]).unwrap().action, Action::Endpoint);
        assert_eq!(args(&["--help"]).unwrap().action, Action::Help);
        assert_eq!(args(&["-V"]).unwrap().action, Action::Version);
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
        for (verb, _) in command::USAGE {
            assert!(text.contains(&format!(" {verb} ")), "help omits {verb}");
        }
        assert!(text.contains(endpoint::CLAUDE_STEM));
    }
}
