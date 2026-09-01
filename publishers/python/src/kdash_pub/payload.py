"""The `ts` rule from contracts/rules.md, applied once.

    Every payload carries `ts` (unix seconds, float) — even when the key also
    has a TTL. Timestamps are the writer's clock.

`now` is always a parameter (CD-10's no-ambient-clock rule); the publisher is
what reads the wall clock. Stdlib only.
"""

from __future__ import annotations

import json
from typing import Any, Mapping


class PayloadError(ValueError):
    """A payload that cannot be published as it stands."""


def stamp(payload: Mapping[str, Any] | str, now: float) -> str:
    """Return the JSON text to write, with `ts` guaranteed present.

    A payload that already carries `ts` keeps it — a publisher replaying an
    observation knows better than the wall clock does. A `ts` that is present
    but not a number is a bug, not a missing field, so it is refused rather
    than overwritten.
    """
    if isinstance(payload, str):
        try:
            loaded = json.loads(payload)
        except json.JSONDecodeError as exc:
            raise PayloadError(f"payload is not JSON: {exc}") from exc
    else:
        loaded = payload

    if not isinstance(loaded, Mapping):
        raise PayloadError("payload is not a JSON object (contracts/rules.md)")

    record = dict(loaded)
    existing = record.get("ts")
    if existing is None and "ts" not in record:
        record["ts"] = stamp_value(now)
    elif isinstance(existing, bool) or not isinstance(existing, (int, float)):
        # `True` is an int in Python and would sail through the numeric check;
        # a boolean timestamp is exactly the sort of thing worth catching here.
        raise PayloadError("payload carries a non-numeric `ts`")

    try:
        return json.dumps(record, separators=(",", ":"), sort_keys=True)
    except (TypeError, ValueError) as exc:
        raise PayloadError(f"payload is not JSON-serialisable: {exc}") from exc


def stamp_value(now: float) -> float:
    """`now` as a `ts` field wants it.

    Rounded to milliseconds: that is the resolution every freshness window in
    the registry is measured against, and a full float of clock noise only
    makes payloads harder to read.
    """
    return round(now, 3)
