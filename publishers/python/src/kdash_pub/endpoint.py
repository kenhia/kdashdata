"""CD-4 endpoint discovery for Python publishers.

The same walk `kdash_endpoint.c` implements for consumers and the Rust wrapper
implements for publishers, delegated to the khlenv Python client rather than
re-derived (CD-11):

1. `$<STEM>` in the environment — an explicit override wins outright.
2. khlenv `<STEM>`. An explicit null here is `NOWHERE` and stops.
3. khlenv `<LEGACY>`, on a **miss** only.
4. the compiled-in default, when khlenv itself could not be reached.

Resolution happens on **every** connect: that is what makes a moved Redis
propagate within one reconnect interval, and what makes the CD-7 cutover a
khlenv store edit rather than a sweep of every publisher host.

The walk itself (`resolve_with`) is pure and importable with nothing installed;
only `resolve` needs khlenv, and it imports it lazily.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from enum import Enum
from typing import Callable, NamedTuple

#: CD-4: the stem every new consumer and publisher resolves.
CENTRAL_STEM = "KDASH_CENTRAL_REDIS"

#: The legacy alias for the same endpoint, kept until kpidash-client's
#: publishers migrate (CD-3/CD-4). Tried only when the new stem MISSES.
CENTRAL_STEM_LEGACY = "KPIDASH_REDIS"

#: Where the central Redis has lived since kpidash 001.
CENTRAL_DEFAULT = "rpi53:6379"

#: CD-7: the stem the claude-feed relocation flips.
CLAUDE_STEM = "KDASH_CLAUDE_REDIS"

REDIS_PORT_DEFAULT = 6379

#: Longest endpoint value khlenv will hand back (matches KDASH_ENDPOINT_MAX).
ENDPOINT_MAX = 256


class EndpointError(ValueError):
    """No usable endpoint could be determined."""


class Answer(Enum):
    """What one khlenv lookup can say — the protocol's four answers."""

    VALUE = "value"
    NULL = "null"
    MISS = "miss"
    UNAVAILABLE = "unavailable"
    REJECTED = "rejected"


class Lookup(NamedTuple):
    answer: Answer
    detail: str = ""


@dataclass(frozen=True)
class Stem:
    """One stem and the walk that belongs to it."""

    key: str
    legacy: str | None = None
    #: What to use when khlenv is unreachable, if anything.
    default: str | None = None


#: The central Redis: legacy alias, and the historical default.
CENTRAL = Stem(CENTRAL_STEM, legacy=CENTRAL_STEM_LEGACY, default=CENTRAL_DEFAULT)

#: The claude family's home — **no default on purpose.**
#:
#: CD-4's default exists because the central Redis has always been at
#: rpi53:6379, and for a *reader* a stale-but-right guess beats nothing. The
#: claude stem is the one being flipped (CD-7), so a compiled-in guess is wrong
#: on one side of the cutover or the other — and a publisher that guesses wrong
#: does not miss a sample, it writes a convincing one to the Redis nobody is
#: reading. A dropped publish is the honest degradation (CD-6).
CLAUDE = Stem(CLAUDE_STEM)

#: Explicit null: khlenv says "deliberately no endpoint". Not an error, and not
#: something to fall back from.
NOWHERE = None


def named_stem(key: str) -> Stem:
    """The stem `key` names, with the well-known walks attached."""
    if key == CENTRAL_STEM:
        return CENTRAL
    if key == CLAUDE_STEM:
        return CLAUDE
    return Stem(key)


def parse_hostport(value: str, defport: int = REDIS_PORT_DEFAULT) -> tuple[str, int]:
    """Split `host` or `host:port`.

    Mirrors `kdash_parse_hostport` and the Rust wrapper, refusals included: an
    IPv6 literal is rejected rather than mis-split, because the homelab
    addresses Redis by name or by IPv4 and silently taking `::1` apart would be
    worse than saying no.
    """
    text = (value or "").strip()
    if not text:
        raise EndpointError("Redis endpoint is empty")
    if len(text) > ENDPOINT_MAX:
        raise EndpointError(f"endpoint {value!r} is longer than khlenv will hand back")

    host, separator, port_text = text.partition(":")
    if not separator:
        return text, defport
    if ":" in port_text:
        raise EndpointError(
            f"endpoint {value!r} looks like an IPv6 literal, which is not supported"
        )
    if not host:
        raise EndpointError(f"malformed endpoint {value!r} — host is empty")
    try:
        port = int(port_text)
    except ValueError:
        raise EndpointError(
            f"malformed endpoint {value!r} — port {port_text!r} is not a number"
        ) from None
    if not 1 <= port <= 65535:
        raise EndpointError(f"malformed endpoint {value!r} — port {port} is out of range")
    return host, port


def resolve_with(
    stem: Stem,
    env: Callable[[str], str | None],
    lookup: Callable[[str], Lookup],
) -> tuple[str, int] | None:
    """The CD-4 walk, with khlenv and the environment supplied by the caller.

    Returns `(host, port)`, or `NOWHERE` for an explicit khlenv null. Split
    from `resolve` so the branching — the part that is actually easy to get
    wrong — is unit-testable with no network and no process environment.
    """
    override = env(stem.key)
    if override and override.strip():
        return parse_hostport(override)

    unavailable = ""
    keys = [stem.key] + ([stem.legacy] if stem.legacy else [])
    for key in keys:
        result = lookup(key)
        if result.answer is Answer.VALUE:
            return parse_hostport(result.detail)
        if result.answer is Answer.NULL:
            # Deliberate: do not fall back to anything, alias or default.
            return NOWHERE
        if result.answer is Answer.MISS:
            continue
        if result.answer is Answer.REJECTED:
            raise EndpointError(f"khlenv rejected {key}: {result.detail}")
        # Unreachable. The alias lives in the same store on the same service:
        # if the service is down, asking twice is a second timeout, not a
        # second chance.
        unavailable = result.detail
        break

    if stem.default:
        return parse_hostport(stem.default)
    if unavailable:
        raise EndpointError(
            f"khlenv unreachable resolving {stem.key}, and this stem has no "
            f"compiled-in default: {unavailable}"
        )
    raise EndpointError(f"khlenv holds no value for {stem.key} at any level")


def resolve(app: str, stem: Stem = CENTRAL) -> tuple[str, int] | None:
    """The live walk: process environment, real khlenv."""
    import khlenv  # imported here so the pure half stays dependency-free

    client = khlenv.Khlenv(app)

    def lookup(key: str) -> Lookup:
        try:
            value = client.get(key)
        except khlenv.KhlenvMiss:
            return Lookup(Answer.MISS)
        except khlenv.KhlenvInvalidKey as exc:
            return Lookup(Answer.REJECTED, str(exc))
        except khlenv.KhlenvError as exc:
            return Lookup(Answer.UNAVAILABLE, str(exc))
        if value is None:
            return Lookup(Answer.NULL)
        return Lookup(Answer.VALUE, value)

    return resolve_with(stem, os.environ.get, lookup)
