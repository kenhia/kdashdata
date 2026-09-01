"""Where the Redis password comes from — CD-2's contract, CD-12's delivery.

CD-2 says the password travels in `REDISCLI_AUTH` and nowhere else, and that
is still the only variable read here. CD-12 adds a second **delivery** route
for the same variable, for contexts systemd's `EnvironmentFile=` cannot reach
(Claude Code hooks and statuslines, where `REDISCLI_AUTH` measures UNSET). Same
variable, same krot entry, same value.

A world- or group-readable secret file is refused, not used.

Stdlib only.
"""

from __future__ import annotations

import os
import stat
from pathlib import Path

#: The one variable that carries the password (CD-2).
AUTH_ENV = "REDISCLI_AUTH"

#: Explicit override naming the env file to read.
AUTH_FILE_ENV = "KDASH_AUTH_FILE"


class AuthError(ValueError):
    """A secret file that exists but must not be used as it stands."""


def parse_env_file(text: str) -> str | None:
    """Pull `REDISCLI_AUTH` out of an `EnvironmentFile`-shaped text.

    The shape is systemd's, because the file this reads is the one systemd
    already reads: `KEY=value`, `#` comments, optional surrounding quotes. The
    last assignment wins, as it does for systemd. An empty assignment is "not
    set" rather than "the empty password".
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


def candidates() -> list[Path]:
    """The env files to try, most specific first.

    The kpidash-client file is last and is not kdashdata's to own — but it is
    already on every reporting host at 0600 and already on krot's
    `rpi53-redis-password` consumer list, so honouring it is what makes CD-12
    work on day one without minting anything or copying a secret around.
    """
    explicit = os.environ.get(AUTH_FILE_ENV, "").strip()
    if explicit:
        return [Path(explicit)]
    config = os.environ.get("XDG_CONFIG_HOME", "").strip()
    base = Path(config) if config else Path.home() / ".config"
    return [
        base / "kdash" / "redis-auth.env",
        base / "kpidash-client" / "redis-auth.env",
    ]


def from_file(path: Path) -> str:
    """Read the password out of one env file, refusing an over-open one."""
    if os.name == "posix":
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & 0o077:
            raise AuthError(
                f"{path} is mode {mode:04o} — a secret file readable by group "
                "or other is refused, not used (chmod 600)"
            )
    value = parse_env_file(path.read_text(encoding="utf-8"))
    if value is None:
        raise AuthError(f"{path} holds no {AUTH_ENV}= line")
    return value


def password() -> str | None:
    """The password, or `None` when there is genuinely none to be had.

    `None` is a normal answer, not a failure: `rpidash2:6380` takes no AUTH
    today. A file that exists but cannot be trusted is the one case that
    raises — silence there would turn a permissions fault into "the feed just
    stopped".
    """
    from_env = os.environ.get(AUTH_ENV, "")
    if from_env:
        return from_env
    for path in candidates():
        if path.exists():
            return from_file(path)
    return None
