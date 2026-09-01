# kdash-pub — Rust publisher wrapper + CLI

The publish side of the [feed contracts](../../contracts/rules.md) for
publishers that cannot afford an interpreter: Claude Code hooks and
statuslines, which fire on every prompt and every tool call. Daemon publishers
use the [Python package](../python/) instead — CD-11 in
[architecture.md](../../docs/architecture.md) has the measurements.

## The CLI

```sh
kdash-pub set   kpidash:services:demo:kai '{"state":"ok","text":"up"}'
kdash-pub setex kdash:selftest:kai 300 '{"host":"kai","publisher":"rust"}'
kdash-pub --stem KDASH_CLAUDE_REDIS --no-auth hset claude:session:kai:abc status working
kdash-pub endpoint          # where would this write, and can it connect?
```

`--no-auth` on the claude line is not decoration: `rpidash2:6380` has **no
password configured**, and sending AUTH to such a server is an error rather
than a no-op. It comes off when CD-7 repoints that stem at central.

Nothing above names a host, a port or a password. `ts` is stamped if the
payload did not carry one, and an off-contract key is refused before a socket
is opened.

### Batch mode — one exec, one connection

```sh
printf 'hset\tclaude:session:kai:abc\tstatus\tworking\nexpire\tclaude:session:kai:abc\t7200\n' \
  | kdash-pub --best-effort batch
```

Commands are **tab-separated**, one per line, and go out in a single pipeline.
Tab because the payloads are JSON and a JSON string cannot contain a literal
tab — so the format needs no quoting rules, and a format with no escaping has
no escaping bugs. Blank lines and `#` comments are skipped.

This mode is what makes the CLI a real replacement for `claude-pub.sh`'s
hand-rolled RESP: a hook that updates a session hash, re-arms its TTL and
pushes a recent record does it in one connection, not three.

### Exit codes, and why a hook wants `--best-effort`

| code | means |
|---|---|
| 0 | published (or `--best-effort` swallowed a delivery failure) |
| 1 | the command is wrong — bad key, bad payload, bad usage |
| 2 | delivery failed — no endpoint, no auth, Redis unreachable |

`--best-effort` turns 2 into 0 and **never** touches 1. A dead Redis must not
fail a hook; a key that violates the grammar is a bug that should be noticed,
and quietly exiting 0 on it is how a publisher goes off-contract for a month
without anyone finding out.

### Options

| flag | |
|---|---|
| `--app <name>` | app name sent to khlenv (default `kdash-pub`) |
| `--stem <KEY>` | which stem to resolve (default `KDASH_CENTRAL_REDIS`; `KDASH_CLAUDE_REDIS` for the claude feed) |
| `--endpoint <h:p>` | pin the endpoint, skipping khlenv entirely |
| `--no-auth` | send no AUTH — required for a Redis with **no password configured** |
| `--best-effort` | see above |
| `--verbose`, `-v` | print the endpoint written to |

## The crate

```rust
use kdash_pub::{Publisher, Stem};

let publisher = Publisher::new("apt-temps", Stem::CENTRAL);
let mut redis = publisher.connect()?;                    // resolves every time
redis.set_expiring("kpidash:client:kai:health", r#"{"ok":true}"#, 5)?;
```

`Publisher` holds no socket; `connect` re-resolves the endpoint on every call,
which is CD-4's propagation rule and what makes the CD-7 cutover a khlenv store
edit rather than a sweep of every publisher host.

## Dependencies

`khlenv-client`, `redis` (sync, no async runtime) and `serde_json`. Argument
parsing is hand-rolled: seven verbs and five flags do not justify a framework
on a binary exec'd once per tool call.

`khlenv-client` is a **git** dependency on a private repo, so a first build
needs network and git credentials for `github.com/kenhia/khlenv`. That cost is
deliberate — CD-11 explains why taking the client beats re-deriving khlenv's
endpoint precedence a third time.

## Development

```sh
just check-rust     # fmt, clippy -D warnings, unit tests
just pub -- --help  # build release and run it
```

The tests cover the pure core with no network and no process environment: key
grammar, the `ts` rule, CD-4's branching (khlenv and the environment are both
injected), the env-file shape, and the CLI's argv front end. The socket half is
verified live instead — `just pub-endpoint`.
