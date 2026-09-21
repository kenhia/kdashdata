//! `kdash-pub` — the Rust publisher wrapper for the kdashdata feed contracts.
//!
//! A publisher's whole job is: find the Redis (CD-4), authenticate (CD-2),
//! write a key that matches the grammar with a payload that matches the `ts`
//! rule (contracts/rules.md). Every publisher in the homelab has re-derived
//! that, and each one has got a different part of it wrong. This crate is that
//! derivation, once.
//!
//! ```no_run
//! use kdash_pub::{Publisher, Stem};
//!
//! # fn main() -> Result<(), Box<dyn std::error::Error>> {
//! let publisher = Publisher::new("kdashdata-demo", Stem::CENTRAL);
//! let mut redis = publisher.connect()?;
//! redis.set_latest(
//!     "kpidash:services:kdashdata-demo:kai",
//!     r#"{"state":"ok","text":"hello from kdash-pub"}"#,
//! )?;
//! # Ok(())
//! # }
//! ```
//!
//! **No rendering, and three reads.** The consumer side is `libkdash` (this
//! repo's `include/kdash/`): the data model, the freshness ladder, the
//! skip-a-bad-record discipline. This crate writes — plus the point reads a
//! publisher needs to guard its own write against clobbering a fresher
//! observation, or to carry a field forward unchanged (CD-14, amended in
//! sprint 015). Bytes and key names, no model, no policy.
//!
//! ## What it does not do
//!
//! It does not decide *when* to publish, cap an event log, or retry. A
//! publisher owns its own cadence and its own cap (rules.md), and a wrapper
//! that retried behind a hook's back would turn a 5 ms fire-and-forget into an
//! unbounded stall.

pub mod auth;
pub mod command;
pub mod endpoint;
pub mod keys;
pub mod payload;

pub use auth::AuthError;
pub use command::{Command, ParseError, Query};
pub use endpoint::{EndpointError, Resolved, Stem};
pub use keys::KeyError;
pub use payload::PayloadError;

use std::fmt;
use std::time::{SystemTime, UNIX_EPOCH};

#[derive(Debug)]
pub enum Error {
    Endpoint(EndpointError),
    Auth(AuthError),
    Parse(ParseError),
    /// khlenv holds an explicit null for this stem: deliberately no endpoint.
    NoEndpoint(String),
    Redis(redis::RedisError),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Endpoint(e) => write!(f, "{e}"),
            Error::Auth(e) => write!(f, "{e}"),
            Error::Parse(e) => write!(f, "{e}"),
            Error::NoEndpoint(stem) => write!(
                f,
                "khlenv holds an explicit null for {stem} — deliberately no \
                 endpoint, so there is nothing to publish to"
            ),
            Error::Redis(e) => write!(f, "redis: {e}"),
        }
    }
}

impl std::error::Error for Error {}

impl From<EndpointError> for Error {
    fn from(e: EndpointError) -> Self {
        Error::Endpoint(e)
    }
}
impl From<AuthError> for Error {
    fn from(e: AuthError) -> Self {
        Error::Auth(e)
    }
}
impl From<ParseError> for Error {
    fn from(e: ParseError) -> Self {
        Error::Parse(e)
    }
}
impl From<redis::RedisError> for Error {
    fn from(e: redis::RedisError) -> Self {
        Error::Redis(e)
    }
}

/// What a [`Connection::read`] came back with.
///
/// Two shapes because the two questions are different: `hget`/`get` ask about
/// one key's contents, `scan` asks which keys exist. Keeping them apart in the
/// type is what stops a caller reading "no keys matched" as "empty value" —
/// which for a presence-owned feed (CD-18) is the difference between "this
/// host is fine" and "this host has been stale for three weeks".
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Answer {
    /// The value, or `None` for an absent key or field.
    Value(Option<Vec<u8>>),
    /// Every key matching the pattern, deduplicated and sorted. Empty is a
    /// complete answer, not a failure.
    Keys(Vec<String>),
}

/// SCAN's per-iteration hint. Large enough that these small keyspaces finish
/// in one or two round trips, small enough not to be a stall of its own.
const SCAN_COUNT: usize = 500;

/// Unix seconds as a `ts` field wants them.
pub fn now() -> f64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs_f64())
        .unwrap_or(0.0)
}

/// A publisher bound to one app name and one stem.
///
/// Cheap to build and cheap to keep: it holds no socket. [`Publisher::connect`]
/// resolves the endpoint **every time**, which is CD-4's propagation rule and
/// what makes the CD-7 cutover a khlenv store edit rather than a sweep of
/// every publisher host.
#[derive(Debug, Clone)]
pub struct Publisher {
    app: String,
    stem: Stem,
    /// Set by [`Publisher::with_endpoint`]: skips discovery entirely.
    pinned: Option<(String, u16)>,
    /// Cleared by [`Publisher::without_auth`] for a Redis that has no password
    /// configured.
    authenticate: bool,
    timeout: std::time::Duration,
}

impl Publisher {
    pub fn new(app: impl Into<String>, stem: Stem) -> Self {
        Publisher {
            app: app.into(),
            stem,
            pinned: None,
            authenticate: true,
            timeout: std::time::Duration::from_millis(1500),
        }
    }

    /// Connect with no AUTH at all.
    ///
    /// Not the same as having no password: sending AUTH to a Redis with none
    /// configured is an **error** (`ERR AUTH <password> called without any
    /// password configured`), so "I found a password" and "this server wants
    /// one" are different questions. The interim claude home
    /// (`rpidash2:6380`) is the live case — it takes no password, which is
    /// why the CD-7 dual-write window needs one publisher able to write both
    /// an authenticated and an unauthenticated endpoint.
    ///
    /// Deliberately explicit rather than a retry-without-password fallback: a
    /// silent retry would turn a *wrong* password into a connection that
    /// succeeds and then fails NOAUTH on every command, which is a worse
    /// failure than the one it papered over.
    pub fn without_auth(mut self) -> Self {
        self.authenticate = false;
        self
    }

    /// Pin the endpoint, skipping khlenv. The most explicit override there is —
    /// above `$<STEM>`, which is itself above the store.
    pub fn with_endpoint(mut self, host: impl Into<String>, port: u16) -> Self {
        self.pinned = Some((host.into(), port));
        self
    }

    /// Bound the connect and the round trip. A publisher on a hook path must
    /// cost a fixed pause when Redis is gone, never an unbounded one (CD-6).
    pub fn with_timeout(mut self, timeout: std::time::Duration) -> Self {
        self.timeout = timeout;
        self
    }

    pub fn app(&self) -> &str {
        &self.app
    }

    pub fn stem(&self) -> &Stem {
        &self.stem
    }

    /// Where this publisher would write right now, without connecting.
    pub fn resolve(&self) -> Result<Resolved, Error> {
        if let Some((host, port)) = &self.pinned {
            return Ok(Resolved::At {
                host: host.clone(),
                port: *port,
            });
        }
        Ok(endpoint::resolve(&self.app, &self.stem)?)
    }

    /// Resolve, authenticate, connect.
    pub fn connect(&self) -> Result<Connection, Error> {
        let (host, port) = match self.resolve()? {
            Resolved::At { host, port } => (host, port),
            Resolved::Nowhere => return Err(Error::NoEndpoint(self.stem.key.to_string())),
        };

        // Resolved once and kept, so the caller can say which file answered
        // (CD-19). This crate does not print it: a library writing to stderr
        // inside a hook is the kind of surprise CD-10 keeps out.
        let auth = if self.authenticate {
            auth::resolve()?
        } else {
            None
        };

        let info = redis::ConnectionInfo {
            addr: redis::ConnectionAddr::Tcp(host.clone(), port),
            redis: redis::RedisConnectionInfo {
                password: auth.as_ref().map(|a| a.password.clone()),
                ..Default::default()
            },
        };
        let client = redis::Client::open(info)?;
        let inner = client.get_connection_with_timeout(self.timeout)?;
        Ok(Connection {
            inner,
            endpoint: (host, port),
            auth: auth.map(|a| (a.source, a.path)),
        })
    }
}

/// A live connection to one Redis, with the publish patterns on it.
pub struct Connection {
    inner: redis::Connection,
    endpoint: (String, u16),
    auth: Option<(auth::Source, Option<std::path::PathBuf>)>,
}

impl Connection {
    /// `host:port` actually connected to — what a `--verbose` publisher prints
    /// and what makes a mis-routed write obvious.
    pub fn endpoint(&self) -> String {
        format!("{}:{}", self.endpoint.0, self.endpoint.1)
    }

    /// Where the password came from, or `None` when none was found or asked
    /// for. The executable reports it; this crate never prints (CD-19, CD-10).
    pub fn auth_source(&self) -> Option<auth::Source> {
        self.auth.as_ref().map(|(source, _)| *source)
    }

    /// The file the password came from, or `None` when `$REDISCLI_AUTH`
    /// answered, nothing did, or auth was not asked for.
    pub fn auth_path(&self) -> Option<&std::path::Path> {
        self.auth.as_ref().and_then(|(_, path)| path.as_deref())
    }

    /// How to name the answering route to a human: the source's label, plus
    /// the file when there was one.
    pub fn auth_origin(&self) -> String {
        match &self.auth {
            None => "no password".to_string(),
            Some((source, None)) => source.label().to_string(),
            Some((source, Some(path))) => format!("{} ({})", source.label(), path.display()),
        }
    }

    /// Latest-value, ts-owned: STRING, no TTL, reader-owned staleness.
    pub fn set_latest(&mut self, key: &str, payload: &str) -> Result<(), Error> {
        self.run(&command::parse(&["set", key, payload], now())?)
    }

    /// Latest-value, expiring: STRING with `SET … EX <ttl>`; key absence is
    /// the liveness signal. rules.md's guidance is TTL ≈ 3× write cadence.
    pub fn set_expiring(&mut self, key: &str, payload: &str, ttl: u64) -> Result<(), Error> {
        self.run(&command::parse(
            &["setex", key, &ttl.to_string(), payload],
            now(),
        )?)
    }

    /// One validated read (CD-14, amended in sprint 015).
    ///
    /// Bytes rather than `String` because a value is whatever some writer put
    /// there. Decoding here would report a non-UTF-8 value as a *delivery*
    /// failure, which is the wrong thing to tell a caller about a Redis that
    /// answered perfectly well.
    pub fn read(&mut self, query: &Query) -> Result<Answer, Error> {
        match query {
            // `Ok(None)` covers both an absent field and an absent key: Redis
            // does not distinguish them, and the guard this exists for treats
            // either as "unknown" and publishes.
            Query::HGet { key, field } => Ok(Answer::Value(
                redis::cmd("HGET")
                    .arg(key)
                    .arg(field)
                    .query::<Option<Vec<u8>>>(&mut self.inner)?,
            )),
            Query::Get { key } => Ok(Answer::Value(
                redis::cmd("GET")
                    .arg(key)
                    .query::<Option<Vec<u8>>>(&mut self.inner)?,
            )),
            Query::Scan { pattern } => Ok(Answer::Keys(self.scan(pattern)?)),
        }
    }

    /// SCAN, cursor to cursor, never KEYS.
    ///
    /// `KEYS` blocks the server for the whole sweep, and this Redis is shared
    /// with the dashboards that render from it — a publisher's convenience is
    /// not worth a visible stall on somebody's panel.
    ///
    /// SCAN gives no uniqueness guarantee across iterations, so the result is
    /// deduplicated; it is sorted because a caller comparing two runs should
    /// be comparing contents, not Redis's internal order.
    fn scan(&mut self, pattern: &str) -> Result<Vec<String>, Error> {
        let mut cursor: u64 = 0;
        let mut found: std::collections::BTreeSet<String> = Default::default();
        loop {
            let (next, batch): (u64, Vec<String>) = redis::cmd("SCAN")
                .arg(cursor)
                .arg("MATCH")
                .arg(pattern)
                .arg("COUNT")
                .arg(SCAN_COUNT)
                .query(&mut self.inner)?;
            found.extend(batch);
            cursor = next;
            if cursor == 0 {
                return Ok(found.into_iter().collect());
            }
        }
    }

    /// One validated command.
    pub fn run(&mut self, command: &Command) -> Result<(), Error> {
        self.pipeline(std::slice::from_ref(command))
    }

    /// Every command in one round trip.
    ///
    /// This is the shape a hook needs: `claude-pub.sh` updates a session hash,
    /// re-arms its TTL and sometimes pushes a recent record, and doing that in
    /// three connects would cost more than the exec it replaced.
    pub fn pipeline(&mut self, commands: &[Command]) -> Result<(), Error> {
        if commands.is_empty() {
            return Ok(());
        }
        let mut pipe = redis::pipe();
        for command in commands {
            match command {
                Command::SetLatest { key, payload } => {
                    pipe.cmd("SET").arg(key).arg(payload).ignore();
                }
                Command::SetExpiring { key, ttl, payload } => {
                    pipe.cmd("SET")
                        .arg(key)
                        .arg(payload)
                        .arg("EX")
                        .arg(ttl)
                        .ignore();
                }
                Command::HSet { key, pairs } => {
                    let mut cmd = redis::cmd("HSET");
                    cmd.arg(key);
                    for (field, value) in pairs {
                        cmd.arg(field).arg(value);
                    }
                    pipe.add_command(cmd).ignore();
                }
                Command::Expire { key, ttl } => {
                    pipe.cmd("EXPIRE").arg(key).arg(ttl).ignore();
                }
                Command::Del { key } => {
                    pipe.cmd("DEL").arg(key).ignore();
                }
                Command::LPush { key, payload } => {
                    pipe.cmd("LPUSH").arg(key).arg(payload).ignore();
                }
                Command::LTrim { key, start, stop } => {
                    pipe.cmd("LTRIM").arg(key).arg(start).arg(stop).ignore();
                }
            }
        }
        pipe.query::<()>(&mut self.inner)?;
        Ok(())
    }
}
