"""The I/O shell: resolve, authenticate, write — and the two reads a publisher
needs to write correctly.

Everything that could be decided without a socket has been (`keys`, `payload`,
`endpoint.resolve_with`); this module is the thin part that talks to Redis.
That is CD-10's split, applied to the publish side.

`redis` is imported lazily so the pure modules stay importable — and testable —
with nothing installed.
"""

from __future__ import annotations

import time
from typing import Any, Mapping

from . import auth, endpoint, keys, payload
from .endpoint import Stem

#: SCAN's per-iteration hint. Large enough that these small keyspaces finish in
#: one or two round trips, small enough not to be a stall of its own.
SCAN_COUNT = 500


class Publisher:
    """A publisher bound to one app name and one stem.

    Cheap to build and cheap to keep: it holds no socket until `connect`, and
    `connect` re-resolves the endpoint **every time** (CD-4).

    ```python
    from kdash_pub import Publisher

    publisher = Publisher("apt-temps")
    publisher.publish_latest(
        "kpidash:apttemps:office", {"zone": "office", "temp_c": 22.4}
    )
    ```
    """

    def __init__(
        self,
        app: str,
        stem: Stem = endpoint.CENTRAL,
        *,
        host: str | None = None,
        port: int = endpoint.REDIS_PORT_DEFAULT,
        authenticate: bool = True,
        timeout: float = 1.5,
    ) -> None:
        self.app = app
        self.stem = stem
        #: Set to pin the endpoint and skip khlenv entirely — the most explicit
        #: override there is, above `$<STEM>`, which is itself above the store.
        self.pinned = (host, port) if host else None
        #: `False` connects with no AUTH at all. Not the same as having no
        #: password: sending AUTH to a Redis with none configured is an
        #: *error* (`ERR AUTH <password> called without any password
        #: configured`), so "I found a password" and "this server wants one"
        #: are different questions. The interim claude home (`rpidash2:6380`)
        #: is the live case. Explicit rather than a retry-without-password
        #: fallback: a silent retry would turn a *wrong* password into a
        #: connection that succeeds and then fails NOAUTH on every command.
        self.authenticate = authenticate
        self.timeout = timeout
        self._client: Any = None
        self._endpoint: tuple[str, int] | None = None
        self._auth: auth.Resolution | None = None

    # --- discovery ---------------------------------------------------------

    def resolve(self) -> tuple[str, int] | None:
        """Where this publisher would write right now, without connecting."""
        if self.pinned:
            return self.pinned
        return endpoint.resolve(self.app, self.stem)

    def connect(self) -> Any:
        """A live `redis.Redis`, re-resolving the endpoint on every call.

        The connection is cached only until the endpoint answer changes: a
        moved Redis is therefore picked up on the next publish, not on the next
        restart.
        """
        import redis  # imported here so the pure half stays dependency-free

        resolved = self.resolve()
        if resolved is endpoint.NOWHERE:
            raise endpoint.EndpointError(
                f"khlenv holds an explicit null for {self.stem.key} — "
                "deliberately no endpoint, so there is nothing to publish to"
            )
        host, port = resolved
        if self._client is not None and self._endpoint == (host, port):
            return self._client

        # Resolved once and kept, so the caller can say which file answered
        # (CD-19). This module does not print it: a library writing to stderr
        # inside a hook is the kind of surprise CD-10 keeps out.
        resolved_auth = auth.resolve() if self.authenticate else None
        self._auth = resolved_auth

        self._client = redis.Redis(
            host=host,
            port=port,
            password=resolved_auth.password if resolved_auth else None,
            socket_connect_timeout=self.timeout,
            socket_timeout=self.timeout,
            decode_responses=True,
        )
        self._endpoint = (host, port)
        return self._client

    @property
    def endpoint(self) -> str | None:
        """`host:port` last connected to — what a log line should carry."""
        return f"{self._endpoint[0]}:{self._endpoint[1]}" if self._endpoint else None

    @property
    def auth(self) -> auth.Resolution | None:
        """Where the password came from on the last `connect()`, or `None`.

        `None` covers three different things — not connected yet, auth not asked
        for, and nothing found — which is why the reporting belongs to a caller
        that knows which of them applies (CD-19).
        """
        return self._auth

    # --- the reads a publisher needs (CD-14, amended sprint 015) -----------
    #
    # Not a consumer API. Anything that wants a FEED — sessions, a staleness
    # verdict, anything rendered — uses libkdash. These two exist because a
    # publisher cannot write correctly without them: `kdash:stale`'s `since`
    # must be read back and carried forward unchanged on every later skip
    # (CD-18), and the identities of that feed are in the KEY, so "which
    # deployers have skipped this host" is a question about keys.

    def get(self, key: str) -> str | None:
        """One STRING key, or `None` when it is not set.

        **`None` and `""` are different answers and the caller must be able to
        tell them apart.** For a presence-owned feed absence is the only
        all-clear (CD-18), so a reader that folds the two together cannot
        implement the contract. `decode_responses=True` on the client means
        this is `str`, not bytes.

        Note what this does NOT do: it raises on a Redis that cannot be
        reached, rather than returning `None`. "I could not ask" is not "the
        key is not set", and a caller that restamps `since` because Redis
        blinked turns "stale for three weeks" into "stale for an hour" while
        looking exactly like working code.
        """
        keys.check_key(key)
        return self.connect().get(key)

    def scan(self, pattern: str) -> list[str]:
        """Every key matching `pattern`, deduplicated and sorted.

        SCAN, cursor to cursor, never KEYS: `KEYS` blocks the server for the
        whole sweep and this Redis is shared with the dashboards rendering
        from it. SCAN gives no uniqueness guarantee across iterations, hence
        the dedupe; the sort is so a caller comparing two runs compares
        contents rather than Redis's internal order.

        An empty list is a complete answer — "no keys matched" — and is not
        the same as a failure, which raises. That distinction is the point:
        an empty result and a suppressed failure are otherwise
        indistinguishable.
        """
        keys.check_pattern(pattern)
        client = self.connect()
        found: set[str] = set()
        cursor = 0
        while True:
            cursor, batch = client.scan(cursor=cursor, match=pattern, count=SCAN_COUNT)
            found.update(batch)
            if cursor == 0:
                return sorted(found)

    # --- the publish patterns (contracts/rules.md) -------------------------

    def publish_latest(self, key: str, record: Mapping[str, Any] | str) -> None:
        """Latest-value, ts-owned: STRING, no TTL, reader-owned staleness."""
        keys.check_key(key)
        self.connect().set(key, payload.stamp(record, time.time()))

    def publish_expiring(
        self, key: str, record: Mapping[str, Any] | str, ttl: int
    ) -> None:
        """Latest-value, expiring: `SET … EX <ttl>`; key absence = offline.

        rules.md's guidance is TTL ≈ 3× write cadence — tight enough that
        staleness means something, loose enough that one missed write is not a
        flap.
        """
        keys.check_key(key)
        if ttl <= 0:
            # SET … EX 0 is an error in Redis; refusing here says which caller
            # asked for it.
            raise ValueError(f"ttl {ttl!r} is not a positive whole number of seconds")
        self.connect().set(key, payload.stamp(record, time.time()), ex=ttl)

    def publish_event(self, key: str, record: Mapping[str, Any] | str, cap: int) -> None:
        """Event log, capped: LPUSH then LTRIM, in one round trip.

        The **writer** owns the cap (rules.md), which is why `cap` is a required
        argument rather than a default: an uncapped list is how a homelab Redis
        quietly fills up.
        """
        keys.check_key(key)
        if cap <= 0:
            raise ValueError(f"cap {cap!r} is not a positive number of entries")
        client = self.connect()
        pipe = client.pipeline(transaction=False)
        pipe.lpush(key, payload.stamp(record, time.time()))
        pipe.ltrim(key, 0, cap - 1)
        pipe.execute()
