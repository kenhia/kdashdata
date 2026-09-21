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
#: feeds; `kpidash`, `claude` and `kvscf` are grandfathered families (CD-3);
#: `ghcp` sits outside `kdash:` by one named exception (CD-21) and mirrors
#: `claude:session` field-for-field; `kdeskdash` and `kstudiodash` are
#: dashboard-local state, listed for visibility and not schema-governed.
#:
#: **scripts/check.py holds this tuple to registry.md in both directions**,
#: because prose and enforcement drifting apart is not hypothetical: sprint 012
#: legalised the `ghcp` family in the schema, the registry and the rules,
#: nothing taught this tuple, and every `ghcp:*` write was refused until sprint
#: 013 (WI 2781). Neither repo's gate could see it, because no gate compared
#: the two sides.
NAMESPACES = (
    "kdash",
    "kpidash",
    "claude",
    "kvscf",
    "ghcp",
    "kdeskdash",
    "kstudiodash",
)

_ALLOWED_EXTRA = frozenset("._-")

#: The glob metacharacters a `scan` pattern may carry inside a segment.
#:
#: Redis patterns also understand `[abc]` classes and `\\` escapes. Both are
#: deliberately refused: a character class is a footgun in a one-line shell
#: argument, and neither buys a publisher anything these two do not. The rule
#: a reader needs is "`*` and `?`, nothing else".
GLOB_CHARS = frozenset("*?")


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


def pattern_segment_ok(segment: str) -> bool:
    """`token_ok`'s charset plus `GLOB_CHARS`, held to a token's length.

    A pattern segment stands in for a token, so it is bounded like one.
    """
    if not segment or len(segment) > TOKEN_MAX:
        return False
    return all(
        c.isascii() and (c.isalnum() or c in _ALLOWED_EXTRA or c in GLOB_CHARS)
        for c in segment
    )


def check_pattern(pattern: str) -> None:
    """Raise `KeyError_` if `pattern` is not one a publisher may SCAN.

    The same grammar as `check_key` with `*` and `?` allowed **inside** a
    segment, and one extra rule that is the whole reason this is its own
    function rather than a flag on `check_key`: **the namespace segment may
    not be globbed.** Relaxing `check_key` to let `*` through anywhere would
    legalise `*:*` — one argument that reads every family on a Redis this repo
    shares with kvscf and the dashboards. A publisher's read is a read of its
    own feed (CD-14, amended in sprint 015).
    """
    if not pattern:
        raise KeyError_("pattern is empty")
    if len(pattern) > KEY_MAX:
        raise KeyError_(f"pattern is {len(pattern)} bytes, over the {KEY_MAX} limit")

    for segment in pattern.split(":"):
        if not segment:
            raise KeyError_(f"pattern {pattern!r} has an empty `:` segment")
        if not pattern_segment_ok(segment):
            raise KeyError_(
                f"pattern segment {segment!r} is not [A-Za-z0-9._-*?] of "
                f"1..{TOKEN_MAX} chars"
            )

    namespace = pattern.split(":", 1)[0]
    if GLOB_CHARS & set(namespace):
        raise KeyError_(
            f"pattern namespace {namespace!r} is globbed — name the family you "
            "are reading. A pattern that crosses families reads keys this "
            "publisher has no contract with, on a Redis it shares"
        )
    if namespace not in NAMESPACES:
        raise KeyError_(
            f"namespace {namespace!r} is not one of {', '.join(NAMESPACES)} — "
            "a feed with no schema in kdashdata is off-contract "
            "(contracts/rules.md)"
        )
