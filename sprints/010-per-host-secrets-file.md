# Sprint 010 — CD-12 reads the per-host secrets file first, on every OS

korg: proposal 2427, work item 2406. One slice of program korg:2440
("simplify homelab secrets"), rank 8 — the first consumer slice after
k-homelab's recipe published the key names (korg:2426, handoff korg:2480).

**Goal.** The publishers' `REDISCLI_AUTH` resolution chain (CD-12) gains the
per-host file k-homelab now renders — `/etc/khomelab/secrets.env` on Linux,
`%ProgramData%\khomelab\secrets.env` on Windows — ahead of the per-user files
CD-12 shipped with, which become deprecated. Both copies of the chain, Python
and Rust, so the fleet does not answer two ways.

## Premise check

| # | claim | verdict |
|---|---|---|
| 2406 | "CD-12's chain is already the resolution mechanism: `$KDASH_AUTH_FILE`, `~/.config/kdash/redis-auth.env`, `~/.config/kpidash-client/redis-auth.env`, consulted only when `REDISCLI_AUTH` is unset" | **holds** — exactly that, in `publishers/python/src/kdash_pub/auth.py` and `publishers/rust/src/auth.rs` |
| 2406 | "the Python publisher **and any Rust/C reader that carries its own copy** of the chain" | **drifted, narrower** — there are **two** copies, not three. The C consumer library reads `getenv(REDISCLI_AUTH)` and nothing else (`src/kdash_conn.c`, `include/kdash/kdash_endpoint.h`); it carries no chain, so there is nothing to keep in step. Left alone deliberately — see "Not done, and why" |
| 2427 notes | "Publish through the package store and **bump the k-homelab pin**" | **gone** — k-homelab pins no `kdash-pub` version. `recipes/claude-hooks/README.md` names `kdash-pub` an *unmanaged prerequisite* installed by `knarr deploy kdash-pub` "because the binary belongs to the repo". The pin that exists is `kdeskdash_publisher_version`, a different artifact. No cross-repo change in this sprint |

## What the premise check turned up that the item did not predict

Measured 2026-09-12, each from the host that runs the publisher there:

| host | `/etc/khomelab/secrets.env` | `ken` in `khomelab`? | readable by the publisher account |
|---|---|---|---|
| kai | present, `root:khomelab 0640` | **no** | **no** — `Permission denied` |
| kubs0 | present, `root:khomelab 0640` | yes | yes |
| cleo | `%ProgramData%` = `C:\ProgramData`; `khomelab\secrets.env` **absent** | n/a | n/a |

Two consequences, and between them they are the whole design of this sprint:

1. **The file is `0640`, and the existing chain refuses it.** Both
   implementations refuse a secret file with any group or other bit
   (`mode & 0o077`), which `0640` has. Adding the path to the candidate list
   and changing nothing else would not "prefer the per-host file" — it would
   make every publisher on every converged host **hard-error**, because the
   walk returns the first existing candidate's error rather than falling
   through.
2. **Group membership is not this repo's to grant.** It is
   `secrets_group_members` in k-homelab's manifest, and kai does not declare
   `ken`. So on kai the file exists and cannot be read, which is the *normal*
   mid-changeover state, not a fault.

## Decisions

### CD-19 — the mode policy travels with the candidate, not its position

Two ownership models, because the fleet has two kinds of secret file:

| model | files | refuses |
|---|---|---|
| private | the per-user files | any group or other bit (`mode & 0o077`) |
| shared | the per-host file, and `$KDASH_AUTH_FILE` | group **write**, any other bit (`mode & 0o027`) |

Group *read* is the per-host file's access mechanism — refusing it would refuse
the contract k-homelab publishes. What stays refused everywhere is group write
(any `khomelab` member could change the password every host reads) and any
world bit (which defeats the group entirely). `0640` passes, `0644` and `0660`
do not. `$KDASH_AUTH_FILE` gets the shared policy because an operator naming a
file explicitly must be able to name *that* file.

**`TooOpen` stays fatal on every rung**, including the shared one. It is the
property CD-12 called load-bearing, and a mis-rendered world-readable fleet
password being silently *used* is worse than publishers stopping.

### CD-19 — two outcomes on the per-host rung mean "keep looking", not "stop"

| outcome on the per-host rung | before | now |
|---|---|---|
| exists, cannot be read (`EACCES` — not in `khomelab` yet) | fatal | skip, keep looking |
| exists, readable, carries no `REDISCLI_AUTH=` line | fatal | skip, keep looking |

Both are properties of a file that is **shared, multi-key and not ours**: nine
keys across eight hosts, rendered by another repo, with each host's manifest
granting a subset. "I am not in the group yet" and "my key is not on this host"
are states of the fleet. On the per-user rungs both stay fatal — those files
exist for exactly one reason, and silence there is the fault CD-12's refusal
was written to make noisy.

Without this, kai's publishers would have stopped the moment this shipped and
stayed stopped until the changeover (korg:2436) added `ken` to `khomelab`.

### The Windows rung is driven by the environment, not by `cfg(windows)`

`ProgramData` is read at run time and the rung is **skipped** when it is unset,
never defaulted to `C:\ProgramData` (program korg:2440's standing rule; Ken,
2026-09-12). Making the rung environment-driven rather than
compile-time-gated is what lets `just check` — which runs on kai, and there is
no Windows CI — prove that nothing is hardcoded by pointing `ProgramData` at a
temp directory. That is WI 2406's second acceptance criterion, satisfied in
the gate rather than by hand on cleo.

### The library does not print; the executable does

Neither wrapper's library has ever written to stderr, and CD-10 is why. So
`resolve()` returns *where the password came from* alongside it, and the
reporting belongs to the thing with a user: `kdash-pub` prints the deprecation
notice and names the answering file under `endpoint`; the Python side exposes
`publisher.auth_source` and `selftest.py` prints it. It also makes "the
per-host file wins, and answering from a per-user file is deprecated" a gate
assertion instead of a stderr scrape.

### `$KDASH_AUTH_FILE` stays exclusive

Set it and no other candidate is considered — the documented contract since
sprint 003, deliberately unchanged. CD-19 adds rungs *below* it, not a
fall-through *out* of it: a caller naming one file means that file.

## What shipped

**The chain, in both wrappers, test for test.** `publishers/rust/src/auth.rs`
and `publishers/python/src/kdash_pub/auth.py` grew a `Source` (environment /
override / per-host / per-user) that carries the mode mask, the
keep-looking rule and the deprecation flag, so the policy lives on the
candidate rather than on its index in a list. 22 Rust and 26 Python auth tests,
`just check` green (54 Rust unit tests, 55 Python, 4 ctest).

**The environment is injected, never mutated.** `candidates_in` / `resolve_in`
in Rust and `candidates(environ=…)` / `resolve(environ=…)` in Python take the
environment as an argument. `cargo test` is multi-threaded, so a test that sets
a variable is a test that flakes its neighbours — and it is what makes the
`ProgramData` rung testable on kai at all.

**Reporting, on the executables.** `kdash-pub` prints `auth from <source>
(<path>)` under `endpoint`/`--verbose`, and warns unconditionally when a
deprecated file answered. Python exposes `publisher.auth` and `selftest.py`
prints the same. `kdash-pub --help`'s description of the chain was the one other
place it was written down, and it says the new order.

**Typed refusals on the Python side.** `TooOpenError` and `NoValueError`, both
still `AuthError` subclasses (asserted, because callers written against sprint
003 catch the base). Without them `resolve_candidates` had to re-read the file
to tell "no key" from "mode not trusted" — a second read of a secret to recover
information the raise already had.

**Docs.** CD-12's heading no longer says "0600" and its convention paragraph
points at CD-19 for the ordering and the mode rule; CD-19 added; the CD-12
section now also records why the **C consumer library deliberately keeps no
chain**. `contracts/rules.md`, `publishers/README.md`,
`publishers/python/README.md` and CLAUDE.md's Status line follow.

### Negative-tested, both languages

Each guard had the error it exists to catch planted, and was watched to fail:

| planted | Rust | Python |
|---|---|---|
| `optional()` always false (the pre-CD-19 fatal reading) | 2 failures | 2 errors |
| `mode_mask()` always `0o077` (refusing the 0640 contract) | 4 failures | 4 errors |
| `ProgramData` defaulted to `C:\ProgramData` instead of skipped | 4 failures | 4 failures |

`scripts/check.py`'s Status-line gate (WI 1928) fired on its own account
mid-sprint, before CLAUDE.md was updated — which is the third negative test,
unplanned.

## Verified live, and from where

Every probe ran **on the host that runs the publisher there**, not from
somewhere else about that host.

**kai** — the interesting host, because `ken` is *not* in `khomelab` here:

```
$ env -u REDISCLI_AUTH just pub --app kdashdata endpoint
rpi53:6379
kdash-pub: auth from per-user env file (deprecated) (…/kpidash-client/redis-auth.env)
kdash-pub: warning: … deprecated, superseded by the per-host secrets file (CD-19)   # exit 0
```

The per-host rung was tried, could not be read, and was skipped — which is the
whole of CD-19's second rule, in the state that actually exists today.

**The control that makes that mean something**: the same file on a
*non-optional* rung must fail, or "skipped" is indistinguishable from "was not
there".

```
$ env -u REDISCLI_AUTH KDASH_AUTH_FILE=/etc/khomelab/secrets.env just pub … endpoint
kdash-pub: /etc/khomelab/secrets.env: Permission denied (os error 13)   # exit 2
```

Python agrees on the same host: `PermissionError` on the override rung,
`per-user env file (deprecated)` on the ordinary walk.

**kubs0** — where `ken` *is* in `khomelab`. The sprint's binary was copied to a
temp directory and run there (not installed, and removed afterwards):

| # | invocation | result |
|---|---|---|
| 1 | `REDISCLI_AUTH` unset, `XDG_CONFIG_HOME` emptied — **only the per-host file reachable** | `auth from per-host secrets file (/etc/khomelab/secrets.env)`, exit 0 |
| 2 | **control**: `REDISCLI_AUTH=definitely-not-the-password` | `Password authentication failed`, exit 2 |
| 3 | the host's normal environment, where **both** files exist | the per-host file answers, no deprecation warning, exit 0 |

Row 2 is what makes row 1 a fact rather than a hope: a wrong password is
refused, so the password that was accepted did the work. Row 3 is the
"with both, the per-host file wins" criterion, live, on a host that has both.

**cleo** — and this went further than expected. The real
`%ProgramData%\khomelab\secrets.env` does not exist there yet, but the *rung*
can be proven anyway, without a real secret, by inverting the control: put a
**deliberately wrong** password in a fake `ProgramData` and require an AUTH
failure. A failure is proof the file was found, read and used.

| probe, run on cleo with the sprint's `.exe` in a temp dir | result |
|---|---|
| `ProgramData` → temp dir holding `khomelab\secrets.env` with a wrong password | `Password authentication failed` — the rung resolved `ProgramData` at run time, found the file, parsed `KEY='value'`, sent it |
| `ProgramData` **unset** | `auth from no password` — skipped, **not** defaulted to `C:\ProgramData` |

The second row is 2406's "never the literal `C:\ProgramData`" requirement,
measured on Windows rather than argued. The deployed
`C:\tools\bin\kdash-pub.exe` was never touched (still `0.1.0-7fe2c87`), and
the probe directory was removed and its absence confirmed.

The cross-compile itself is also verified: `cargo build --release --target
x86_64-pc-windows-gnu` and clippy both clean, which is the only place the
`cfg(not(unix))` mode-check branch is compiled at all.

No secret value was printed at any point — every check reports the *source and
path*, and the fingerprints of nothing else.

## Not done, and why

- **cleo against the REAL file — WI 2491, with soak fields.** The mechanism is
  proven on cleo (above); what is left is only that the real
  `C:\ProgramData\khomelab\secrets.env` holds the right value and
  authenticates — and it does not exist there yet, because cleo is not one of
  k-homelab's eight hosts. The file arrives with the changeover (korg:2436) or
  whichever slice takes cleo on. `check_after 2026-09-13`, `invalidated_if`
  naming the Windows path or the key name changing before cleo is converged.
  Filed rather than left open on 2406, because an open implementation item reads
  as "somebody should be working this" and nobody can be.
- **`kdash-pub endpoint` returns 0 with no credential at all — WI 2492.**
  Measured on kubs0 as the third control: `--no-auth` against the authenticated
  rpi53 exits 0, because `endpoint` connects and issues no command, and Redis
  only rejects AUTH that is *sent*. Pre-dates this sprint; surfaced by CD-19
  making the answering rung visible. Filed and not repaired because the fix
  changes what exit 0 means for every existing caller of a published CLI —
  that is a contract decision, and the item names it.
- **Group membership on kai was not granted.** It is
  `secrets_group_members` in k-homelab's manifest, not kdashdata's to write,
  and it is korg:2436's work. CD-19 is designed so that this costs nothing: kai
  keeps publishing from the deprecated file and says so.
- **Nothing is published or deployed.** `just publish` refuses a dirty tree and
  will not move `latest` off `main`, so it follows the merge — see below.

## Repaired in passing

Nothing. The gate was green on arrival apart from the Status line this sprint
was obliged to write, and no doc outside the ones listed above claimed the old
chain (checked by grep for `0600 env file` and for `KDASH_AUTH_FILE`).

## Deployed

Not yet. After korg:2427 merges, from `main` on kai:

```sh
just publish      # linux + windows, one version, to the package store
just deploy-all   # kai, kubs0 (knarr) and cleo (install-cleo.ps1)
```

`deploy-all` and not `deploy`: kpolice sprint 002's lesson is in the justfile —
never verify by iterating the hosts you deployed. Then the live check on each of
the three, with the wrong-password control beside it, and cleo's is WI 2491.
