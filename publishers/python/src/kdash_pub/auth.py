"""Where the Redis password comes from — CD-2's contract, CD-12's delivery,
CD-19's per-host file.

CD-2 says the password travels in `REDISCLI_AUTH` and nowhere else, and that
is still the only variable read here. CD-12 added a second **delivery** route
for the same variable, for contexts systemd's `EnvironmentFile=` cannot reach
(Claude Code hooks and statuslines, where `REDISCLI_AUTH` measures UNSET).

CD-19 puts the fleet's own per-host file at the front of that route:
`/etc/khomelab/secrets.env` on Linux, `%ProgramData%\\khomelab\\secrets.env` on
Windows, rendered by k-homelab from the age store. The per-user files CD-12
shipped with are still read, and are now deprecated.

Two rules are load-bearing rather than tidy, and both belong to the **source** a
candidate came from rather than to its path:

- A secret file anything untrusted can read, or anything can write, is refused
  rather than used — but what counts as untrusted differs. A per-user file is
  ours and 0600, so any group or other bit is a fault; the per-host file is
  `root:khomelab 0640` and group read is the whole access mechanism. See
  `Source.mode_mask`.
- The per-host file is shared, and not ours: nine keys, eight hosts, rendered
  by another repo, each host's manifest granting a subset. So "I cannot read it
  yet" and "it does not carry my key" are states of the fleet, not faults — they
  keep looking. See `Source.optional`.

Nothing here prints. `resolve()` reports *where* the password came from and the
caller does the telling (CD-10).

Stdlib only.
"""

from __future__ import annotations

import enum
import os
import stat
from pathlib import Path
from typing import Mapping, NamedTuple

#: The one variable that carries the password (CD-2).
AUTH_ENV = "REDISCLI_AUTH"

#: Explicit override naming the env file to read. Set it and no other
#: candidate is considered.
AUTH_FILE_ENV = "KDASH_AUTH_FILE"

#: The per-host file k-homelab renders on Linux: `root:khomelab 0640`,
#: `KEY='value'` lines (CD-19).
PER_HOST_FILE = "/etc/khomelab/secrets.env"

#: The Windows variable naming the per-machine data directory. Read at RUN TIME
#: and never defaulted: `C:\\ProgramData` is what it resolves to on Ken's
#: machines today, but a VM elsewhere routinely resolves it to another drive
#: (Ken, 2026-09-12). Unset means the rung is skipped, not guessed.
PROGRAMDATA_ENV = "ProgramData"


class AuthError(ValueError):
    """A secret file that exists but must not be used as it stands."""


class TooOpenError(AuthError):
    """The file's mode is not one its source may be trusted at.

    Fatal on every rung, including the shared one: silently *using* a
    world-readable fleet password is worse than a publisher failing loudly.
    """


class NoValueError(AuthError):
    """The file was read and carries no `REDISCLI_AUTH=` line.

    Fatal on a per-user file — it exists for exactly one reason. Skipped on the
    shared per-host file, which carries nine keys and grants a subset per host.
    """


class Source(enum.Enum):
    """Where a candidate came from.

    It fixes two things a path cannot: the permissions the file must have, and
    what an unusable one *means*.
    """

    #: `$REDISCLI_AUTH` answered. Never a file, so the file policy never
    #: applies to it.
    ENVIRONMENT = "REDISCLI_AUTH"
    #: `$KDASH_AUTH_FILE` — a caller named this file explicitly.
    OVERRIDE = "KDASH_AUTH_FILE"
    #: The per-host file k-homelab renders (CD-19).
    PER_HOST = "per-host secrets file"
    #: The per-user files CD-12 shipped with. Deprecated: the k-homelab
    #: changeover deletes them.
    PER_USER = "per-user env file (deprecated)"

    @property
    def mode_mask(self) -> int:
        """Mode bits that make a secret file from this source untrustworthy.

        A per-user file is single-purpose, ours, and was minted 0600, so ANY
        group or other bit is a fault. The per-host file's published contract is
        `0640 root:khomelab`: group **read** is how a publisher reaches it at
        all, so refusing that would refuse the contract. What stays refused
        everywhere is group **write** — any `khomelab` member could change the
        password every host reads — and any **other** bit, which defeats the
        group entirely. `0640` passes; `0644` and `0660` do not.
        """
        return 0o077 if self is Source.PER_USER else 0o027

    @property
    def optional(self) -> bool:
        """Whether an unusable file here means "keep looking" rather than "stop".

        Only the per-host file, and only for the two outcomes `resolve` names:
        it is shared with nine keys across eight hosts and rendered by another
        repo, so not being able to read it and it not carrying our key are both
        states of the fleet. Measured 2026-09-12: `ken` is in `khomelab` on
        kubs0 and is **not** on kai, so a fatal reading here would have stopped
        kai's publishers between this slice and the changeover (korg:2436).

        A mode that cannot be trusted is *not* covered by this — that is a
        fault, and it still stops.
        """
        return self is Source.PER_HOST

    @property
    def deprecated(self) -> bool:
        """Answering from here still works and should stop: the k-homelab
        changeover deletes these files."""
        return self is Source.PER_USER

    @property
    def label(self) -> str:
        """How to name this source to a human."""
        return self.value


class Candidate(NamedTuple):
    """One file to try, and the source whose rules it plays by."""

    path: Path
    source: Source


class Resolution(NamedTuple):
    """The password, and where it came from."""

    password: str
    source: Source
    #: The file that answered, or `None` when `$REDISCLI_AUTH` did.
    path: Path | None = None

    def origin(self) -> str:
        """How to name the answering route to a human."""
        return f"{self.source.label} ({self.path})" if self.path else self.source.label


def parse_env_file(text: str) -> str | None:
    """Pull `REDISCLI_AUTH` out of an `EnvironmentFile`-shaped text.

    The shape is systemd's, because the files this reads are ones systemd
    already reads: `KEY=value`, `#` comments, optional surrounding quotes. The
    last assignment wins, as it does for systemd. An empty assignment is "not
    set" rather than "the empty password".

    k-homelab writes the per-host file as `KEY='value'` deliberately — single
    quotes are the one form bash, systemd `EnvironmentFile=`, docker compose
    `env_file:` and python-dotenv all read identically (handoff korg:2480) — and
    that is already the quoting this strips.
    """
    found = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        # Tolerate the `Environment=REDISCLI_AUTH=…` spelling: that is what the
        # pre-WI-255 units held, and k-homelab's migration copies it verbatim.
        line = line.removeprefix("Environment=")
        name, separator, value = line.partition("=")
        if not separator or name.strip() != AUTH_ENV:
            continue
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        if value:
            found = value
    return found


def candidates(environ: Mapping[str, str] | None = None) -> list[Candidate]:
    """The files to try, most specific first.

    `environ` is injectable so the ordering, the `ProgramData` rung and the
    override's exclusivity are testable without mutating the process
    environment.
    """
    env = os.environ if environ is None else environ

    def setting(key: str) -> str:
        return env.get(key, "").strip()

    # Unchanged contract (sprint 003): an explicit override is EXCLUSIVE. CD-19
    # adds rungs below it, not a fall-through out of it — a caller naming one
    # file means that file.
    explicit = setting(AUTH_FILE_ENV)
    if explicit:
        return [Candidate(Path(explicit), Source.OVERRIDE)]

    out: list[Candidate] = []

    # The per-host file (CD-19). Driven by the environment rather than by the
    # platform on purpose: `ProgramData` is unset on Linux, so the Windows rung
    # costs nothing there, and pointing it at a temp directory is what lets the
    # gate prove nothing is hardcoded on the host `just check` actually runs on
    # (WI 2406's second acceptance criterion).
    programdata = setting(PROGRAMDATA_ENV)
    if programdata:
        out.append(Candidate(Path(programdata) / "khomelab" / "secrets.env", Source.PER_HOST))
    out.append(Candidate(Path(PER_HOST_FILE), Source.PER_HOST))

    # The deprecated per-user files. The kpidash-client one is last and is not
    # kdashdata's to own — but it is already on every reporting host at 0600 and
    # already on krot's `rpi53-redis-password` consumer list, which is what made
    # CD-12 work on day one without minting anything.
    config = setting("XDG_CONFIG_HOME")
    home = setting("HOME")
    base = Path(config) if config else (Path(home) / ".config" if home else None)
    if base is not None:
        out.append(Candidate(base / "kdash" / "redis-auth.env", Source.PER_USER))
        out.append(Candidate(base / "kpidash-client" / "redis-auth.env", Source.PER_USER))

    return out


def from_file(path: Path, source: Source = Source.PER_USER) -> str:
    """Read the password out of one env file, under one source's rules."""
    if os.name == "posix":
        # Windows has no mode bits to check; the per-host file there relies on
        # the ACLs `%ProgramData%\\khomelab\\` carries.
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & source.mode_mask:
            want = (
                "chmod 600"
                if source is Source.PER_USER
                else "0640 root:khomelab at most — group read is the access "
                "mechanism, group write and world read are not"
            )
            raise TooOpenError(
                f"{path} is mode {mode:04o} — a secret file this open is "
                f"refused, not used ({want})"
            )
    value = parse_env_file(path.read_text(encoding="utf-8"))
    if value is None:
        raise NoValueError(f"{path} holds no {AUTH_ENV}= line")
    return value


def resolve_candidates(candidate_list: list[Candidate]) -> Resolution | None:
    """Walk a candidate list, first answer wins.

    Separated from `resolve` so the skip-and-keep-looking rules are testable
    against a temp directory without touching the process environment.
    """
    for candidate in candidate_list:
        if not candidate.path.exists():
            continue
        try:
            password = from_file(candidate.path, candidate.source)
        except (OSError, NoValueError):
            # A shared file we cannot read yet (`EACCES` — not in `khomelab`),
            # or one that does not carry our key, is the fleet mid-changeover
            # rather than a fault. On a file that is ours, both still stop. A
            # `TooOpenError` is not caught here and so stops on every rung.
            if candidate.source.optional:
                continue
            raise
        return Resolution(password, candidate.source, candidate.path)
    return None


def resolve(environ: Mapping[str, str] | None = None) -> Resolution | None:
    """The password and where it came from, or `None` when there is genuinely
    none to be had.

    `None` is a normal answer, not a failure: `rpidash2:6380` takes no AUTH
    today. A file that exists but cannot be trusted is the one case that
    raises — silence there would turn a permissions fault into "the feed just
    stopped".
    """
    env = os.environ if environ is None else environ
    from_env = env.get(AUTH_ENV, "")
    if from_env:
        return Resolution(from_env, Source.ENVIRONMENT, None)
    return resolve_candidates(candidates(env))


def password(environ: Mapping[str, str] | None = None) -> str | None:
    """The password alone. `resolve` is the one to reach for when the caller can
    report which file answered."""
    found = resolve(environ)
    return found.password if found else None
