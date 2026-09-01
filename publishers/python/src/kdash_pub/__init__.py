"""kdash-pub — the Python publisher wrapper for the kdashdata feed contracts.

A publisher's whole job is: find the Redis (CD-4), authenticate (CD-2), write a
key that matches the grammar with a payload that matches the `ts` rule
(contracts/rules.md). This package is that derivation, once, for the daemon
publishers. Shell publishers on a latency-sensitive path use the Rust CLI
instead (`kdash-pub`, this repo's `publishers/rust/`) — see CD-11.

**No rendering, no reading.** The consumer side is `libkdash`.

    from kdash_pub import Publisher

    publisher = Publisher("apt-temps")
    publisher.publish_expiring(
        "kpidash:client:kai:health", {"ok": True}, ttl=5
    )

`Publisher` needs `redis` installed; the pure modules (`keys`, `payload`,
`auth`, and `endpoint.resolve_with`) import nothing but the stdlib, which is
what lets the repo's gate test them on a host with nothing installed. That
split is why the import below is lazy rather than eager: importing this package
must not require `redis` unless a `Publisher` is actually built.
"""

from __future__ import annotations

from typing import TYPE_CHECKING

from . import auth, endpoint, keys, payload
from .endpoint import CENTRAL, CLAUDE, EndpointError, Stem
from .keys import KeyError_
from .payload import PayloadError

if TYPE_CHECKING:  # pragma: no cover
    from .publisher import Publisher

__version__ = "0.1.0"

__all__ = [
    "CENTRAL",
    "CLAUDE",
    "EndpointError",
    "KeyError_",
    "PayloadError",
    "Publisher",
    "Stem",
    "auth",
    "endpoint",
    "keys",
    "payload",
]


def __getattr__(name: str):
    """Import `Publisher` only when it is asked for (PEP 562)."""
    if name == "Publisher":
        from .publisher import Publisher

        return Publisher
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
