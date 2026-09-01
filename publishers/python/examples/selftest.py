#!/usr/bin/env python3
"""A demo publisher — WI #1746's acceptance criterion, made executable.

Writes `kdash:selftest:<host>` to the central Redis: a schema-valid
latest-value feed, published with **zero hardcoded endpoints** and zero
hardcoded credentials. Everything it needs it asks for — khlenv for the
endpoint (CD-4), `REDISCLI_AUTH` or a 0600 env file for the password
(CD-2/CD-12), `contracts/schemas/kdash-selftest.schema.json` for the shape.

It is also useful past the sprint: run it on any host to find out whether that
host can publish at all, and what it would publish to.

    python3 publishers/python/examples/selftest.py
    python3 publishers/python/examples/selftest.py --dry-run   # no write

Needs `khlenv` and `redis` installed; the repo's gate deliberately does not.
"""

from __future__ import annotations

import argparse
import socket
import sys

from kdash_pub import Publisher, __version__
from kdash_pub.endpoint import NOWHERE

APP = "kdashdata"
TTL_S = 300  # ~3x a five-minute self-test cadence, per rules.md's guidance


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="resolve, but do not write")
    parser.add_argument("--note", default="publisher self-test", help="free-text note field")
    args = parser.parse_args()

    host = socket.gethostname().split(".")[0].lower()
    publisher = Publisher(APP)

    resolved = publisher.resolve()
    if resolved is NOWHERE:
        print(f"{publisher.stem.key}: explicit null — deliberately no endpoint", file=sys.stderr)
        return 2
    print(f"endpoint: {resolved[0]}:{resolved[1]}")

    key = f"kdash:selftest:{host}"
    record = {
        "host": host,
        "publisher": "python",
        "version": __version__,
        "note": args.note,
    }
    if args.dry_run:
        print(f"would write {key} (ttl {TTL_S}s): {record}")
        return 0

    publisher.publish_expiring(key, record, ttl=TTL_S)
    print(f"wrote {key} (ttl {TTL_S}s) to {publisher.endpoint}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
