"""Pure key grammar for the publish side — contracts/rules.md's choke point.

The same rules the Rust wrapper enforces and the same rules `kdash_keys.h`
parses with. Three implementations that disagree would be three contracts, so
the tests on each side pin the same table of accepted and refused keys.

Stdlib only, on purpose: this module has to be importable — and testable — on
a host where neither `redis` nor `khlenv` is installed.
"""

from __future__ import annotations

#: Host/session token contract (rules.md): [A-Za-z0-9._-], 1..63 chars.
TOKEN_MAX = 63

#: Widest key any governed family produces. Generous rather than tight: the
#: limit refuses something pathological, it does not police length.
KEY_MAX = 512

#: The namespaces contracts/registry.md knows about. `kdash` is for new shared
#: feeds; the next three are grandfathered families (CD-3); the last two are
#: dashboard-local state, listed for visibility and not schema-governed.
NAMESPACES = (
    "kdash",
    "kpidash",
    "claude",
    "kvscf",
    "kdeskdash",
    "kstudiodash",
)

_ALLOWED_EXTRA = frozenset("._-")


class KeyError_(ValueError):
    """A key that violates the grammar. Named to avoid shadowing builtins."""


#: The public name. `kdash_pub.keys.KeyError` shadows the builtin only inside
#: this module's own namespace, which is where readers expect it to mean this.
KeyError = KeyError_  # noqa: A001 - deliberate, see above


def token_ok(token: str) -> bool:
    """True when `token` satisfies the rules.md token contract.

    Mixed case is accepted deliberately, because the consumer library accepts
    it: hostnames and session ids arrive from `hostname` and from Claude Code,
    and a publisher stricter than its reader would refuse keys that work.
    """
    if not token or len(token) > TOKEN_MAX:
        return False
    return all(c.isascii() and (c.isalnum() or c in _ALLOWED_EXTRA) for c in token)


def check_key(key: str) -> None:
    """Raise `KeyError_` if `key` is not one a publisher may write."""
    if not key:
        raise KeyError_("key is empty")
    if len(key) > KEY_MAX:
        raise KeyError_(f"key is {len(key)} bytes, over the {KEY_MAX} limit")

    for segment in key.split(":"):
        if not segment:
            raise KeyError_(f"key {key!r} has an empty `:` segment")
        if not token_ok(segment):
            raise KeyError_(
                f"key segment {segment!r} is not [A-Za-z0-9._-] of "
                f"1..{TOKEN_MAX} chars"
            )

    # Namespaces are lowercase by rules.md, and the check is exact: `Claude`
    # and `claude` are different keys to Redis, so accepting either would put
    # two families in one namespace.
    namespace = key.split(":", 1)[0]
    if namespace not in NAMESPACES:
        raise KeyError_(
            f"namespace {namespace!r} is not one of {', '.join(NAMESPACES)} — "
            "a feed with no schema in kdashdata is off-contract "
            "(contracts/rules.md)"
        )
