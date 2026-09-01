# kdash-pub — Python publisher wrapper

The publish side of the [feed contracts](../../contracts/rules.md), for the
homelab's **daemon** publishers. Shell publishers on a latency-sensitive path
(Claude Code hooks, statuslines) use the [Rust CLI](../rust/) instead — see
CD-11 in [architecture.md](../../docs/architecture.md).

```python
from kdash_pub import Publisher

publisher = Publisher("apt-temps")

# latest-value, expiring — key absence is the liveness signal
publisher.publish_expiring("kpidash:client:kai:health", {"ok": True}, ttl=5)

# latest-value, ts-owned — the reader owns the staleness window
publisher.publish_latest("kpidash:apttemps:office", {"zone": "office", "temp_c": 22.4})

# event log, capped — the writer owns the cap, so it is a required argument
publisher.publish_event("kpidash:activities", {"what": "deploy"}, cap=20)
```

Nothing above names a host, a port or a password. The endpoint comes from
khlenv on **every** connect (CD-4), the password from `REDISCLI_AUTH` or, when
that is unset, a 0600 env file (CD-2/CD-12). `ts` is stamped if the payload did
not carry one, and the key is checked against the grammar before anything is
written.

## Install

```toml
dependencies = ["kdash-pub>=0.1.0"]

# khlenv and kdash-pub are published to the homelab package store, not PyPI.
[[tool.uv.index]]
name = "homelab"
url = "https://kubsdb.encke-wahoo.ts.net:4880/simple/"
```

## What it does not do

It does not decide **when** to publish, cap an event log for you, or retry. A
publisher owns its own cadence and its own cap. It does not read, either — the
consumer side is `libkdash` ([`include/kdash/`](../../include/kdash/)).

## Pure core, thin shell

`keys`, `payload`, `auth` and `endpoint.resolve_with` import **nothing but the
stdlib**, and hold everything that is actually easy to get wrong: the key
grammar, the `ts` rule, the env-file shape, and CD-4's branching. `publisher`
and `endpoint.resolve` are the shell, and import `redis` and `khlenv` lazily.

That is CD-10's split, and it is what lets the repo's gate test this package on
a host with nothing installed:

```sh
just check-python          # or: PYTHONPATH=src python3 -m unittest discover -s tests
```

## Self-test

[`examples/selftest.py`](examples/selftest.py) publishes
`kdash:selftest:<host>` — a real feed with a
[schema](../../contracts/schemas/kdash-selftest.schema.json), read by nothing —
so "can this host publish, and where to?" has a one-command answer. It needs
`khlenv` and `redis` installed, which the gate does not.
