# Sprint 014 — The dev pair moves to central, and panels take commands from it

korg: proposal 2931. Slice 1 of program korg:2935 ("Two Redis servers: central
on rpi53, and rpidash3's for kwork"). Run as an overseen karc leg on kai.

**Goal.** A contract slice, and the only one of the program's four that is
kdashdata's. The fleet is going from five Redis servers to two, and three of
the three being retired are load-bearing for kdeskdash: `rpidash2:6380` carries
the dev pair's `kvscf:*`, and both panels' loopback `:6379` carries their
durable state *and* their remote control. Slices 2–4 do the moving; this one
decides what they are moving to and writes it down, so a kdeskdash slice never
has to invent a contract while it is in the middle of a repoint.

Two decisions, one amended and two written new:

1. **CD-8 amended.** `kvscf:*:cleo` moves to central; the kwork pair stays on
   `rpidash3:6380` with its own password, and that is not a leftover.
2. **The panel-control family grows two siblings** — `kdash:panelmode:{host}`
   and `kdash:panelshot:{host}` — so a panel with no local Redis can still be
   switched, configured and screenshotted (CD-22).
3. **The fleet password is a control-plane credential**, said out loud at the
   point it became true rather than the point it bites (CD-23).

## Premise check

The proposal carries no covered work items, so what was checked is its own
`notes` — every falsifiable claim in it, against the two consumer repos on this
host and this repo's own contracts.

| claim | verdict |
|---|---|
| `kvscf:*` keys are already host-scoped, so central has no collision | **holds** — `instances`, `edge`, `apps`, `launcher`, `focus` all end in `<host>` (kdeskdash `src/kvscf_redis.c`) |
| both panels pin `KDESKDASH_KVSCF_REDIS_HOST/PORT` explicitly | **holds** — `src/config.c`, with the claude values as the fallback |
| kdeskdash needs mode / screenshot / GoL–GoLZ settings from central | **holds** — `src/redis.h` documents all three: `active_mode` polled, `gol:settings` and `golz:settings` as HGETALL+DEL, `screenshot` as GETDEL with a `/`-prefixed path |
| "extend `kdash:panel:{host}` **or** add sibling keys" | **drifted, decisively toward siblings** — see CD-22; `want` is a required closed enum with a **live** reader (kstudiodash `src/feeds.c`, `src/run.c`), and rules.md answers a retype-in-place with a new family segment, not a mutation |
| "C reader only if the existing `kdash_panel()` cannot carry the new commands" | **it cannot**, and the readers have an immediate consumer: kdeskdash already links libkdash as a submodule for the `claude:*` readers |
| the schema/allowlist gate would need a publisher edit for a new family | **does not** — `scripts/check.py` compares *namespaces* (`## Family:` headings), and both siblings live under the `kdash` namespace the wrappers already accept |

One claim was not re-measured and is taken from the program's own inventory
(korg WI 2479 comment 2649): that cleo and rpidash2 already hold the central
password. That is a secrets fact, it belongs to the k-homelab slice that will
act on it, and re-deriving it here would have meant reading credentials this
sprint has no use for.

## Decisions

### CD-8, amended — the dev pair's half moves, the work pair's does not

The *shape* argument in CD-8 survives: kvscf data really is a two-machine
exchange rather than fleet state. What does not survive is the topology
conclusion, because it was buying pair-local scoping at the price of a Redis
server per pair. The split now follows the line that actually matters — which
machine can reach central. kwork is LAN-only and MS-managed, so its pair keeps
its own instance and its own password; cleo's does not need to.

Three costs are recorded with the decision rather than left to be discovered:

- the home desk launcher now depends on rpi53 being up;
- **the pairing token's audience widens from two machines to every holder of
  the fleet password**, because `kvscf:focus:<host>` carries it in the payload.
  This one has no topology answer — keeping the channel off central means
  keeping the server the program exists to retire — so the answer is the
  rotatable per-host token (korg WI 2479) and OQ-2. What bounds it today: a
  launcher button carries a stable `key` and never a command line, so the
  executable mapping stays on cleo;
- **the pair-local Redis *was* the scoping.** The panels discover with `SCAN
  kvscf:instances:*`, which returned one workstation's keys because only one
  workstation wrote to that instance. On central the host segment is the only
  scoping left, so a panel's pair host is configuration, not discovery.

The middle one is the finding this sprint did not expect to make, and it is
raised for the overseer rather than buried here.

### CD-22 — one control family per verb

The proposal left the shape open. Three independent reasons close it the same
way.

**One `ts` is one command's identity.** CD-17 made the stamp *the command*. A
payload carrying two commands carries two identities under one stamp, so acting
on either means re-acting on the other; per-field acted stamps would be sibling
keys smuggled inside one payload.

**Widening `want` is a breaking change the rules forbid in place.** It is a
closed enum with a live consumer. rules.md answers a retype with
`kdash:panel:v2:…`, which would move the one consumer this family has in order
to serve a second one that does not need it. There is also a C-level trap in
the alternative: `kdash_panel_t.want`'s zero value is `KDASH_PANEL_DASH`, a
real command, so relaxing `want` to optional would make a `want`-less record
parse into "show the dashboard".

**They are two axes.** `want` is the coarse question every dashboard host has —
is the dashboard on the screen, or has it yielded the display. `mode` is which
screen *within* the dashboard. kstudio answers only the first, a desk panel
only the second, and a dashboard that one day answers both reads two keys that
compose rather than one that has to mean both.

The part that generalises beyond this sprint: **vocabulary belongs to the
dashboard, shape belongs to the contract.** `mode` is not a closed enum and
`settings` is not a field list, because enumerating three dashboards' screens
and every mode's tuning knobs here would mean editing a fleet contract every
time a panel gains a view or a slider.

Settings ride the mode command rather than getting a third family — they are
always *a mode's* settings, and "show golz, seeded like this" is one command
the old plant-the-hash-then-switch dance could only approximate. The capability
that costs: a hash could be planted *in advance* and picked up whenever that
mode next started, and a 60 s edge window cannot express that.

**Rejected, recorded so it is not re-proposed:** `kdash:panelstate:{host}`, a
one-way visibility feed published by the panel. Nothing reads it — the reader
imagined for it was a human at a `redis-cli` — and it would put a write path on
a consumer for visibility alone, which is CD-5's property spent on nothing.

### CD-23 — the fleet password commands panels

`kdash:panel:*` already meant that anyone holding `REDISCLI_AUTH` could switch
kstudio's screen. This sprint adds capture-the-screen with a caller-supplied
path, and CD-8's amendment puts cleo's focus channel on the same instance.
Accepted rather than mitigated per feed, because a per-feed guard cannot work:
the writers are ordinary fleet hosts running `kdash-pub`, and a guard the
wrapper enforces is one `redis-cli` walks past. The real split is **OQ-2**,
which is why that question is still open.

What does belong on the consumer's side, and is in the contract: `path` is
absolute so it can never resolve against a working directory, and a dashboard
should refuse one outside a directory it owns.

## What shipped

### The contract

- `contracts/schemas/kdash-panelmode.schema.json` — `mode` (required,
  `^[a-z][a-z0-9_-]{0,30}$`), optional `settings` (≤24 entries, names
  `^[a-z][a-z0-9_]{0,30}$`, values text ≤63 chars), `ts` required and positive.
- `contracts/schemas/kdash-panelshot.schema.json` — optional absolute `path`
  (≤255), `ts` required and positive.
- `contracts/registry.md` — both families in the `kdash` table, the three-verb
  cluster written out, the `kvscf` section rewritten around its two homes, the
  Homes row and the `kdeskdash:*` local row updated.
- `docs/architecture.md` — CD-8 amended, CD-22 and CD-23 added, CD-1's "one
  exception remains" line corrected to say it is now half the size it was.

Schema bounds were chosen to match the C buffers exactly (31 / 31 / 63 / 255
characters), so a conforming record always fits and a non-conforming one is
rejected rather than truncated into place.

### The library

Readers, because the proposal's own conditional fires and kdeskdash already
links libkdash:

- `kdash_panelmode_key()` / `kdash_panelshot_key()`, plus a shared
  `prefixed_host_key()` that `kdash_panel_key()` now also uses — one
  implementation of the same three-segment construction instead of three.
- `kdash_parse_panelmode()` / `kdash_parse_panelshot()`, and
  `kdash_setting_get()`, whose NULL is the contract: a name the command did not
  carry keeps whatever the mode would have used.
- **`kdash_cmd_actionable()`** — CD-17's edge rule, family-agnostic.
  `kdash_panel_actionable()` is now this function with the record unwrapped, so
  the three families cannot drift on "have I acted on this already".
- `kdash_panelmode()` / `kdash_panelshot()` readers, sharing `panel_cmd_get()`
  with `kdash_panel()`.
- `kdash_dump` prints all three verbs, so `just dump` still exercises
  everything the library can read.

Each verb keeps its **own** acted stamp. Sharing one across the three would
make acting on any command suppress the next one of a different kind, and
there is a test that says so.

### Tests

`tests/test_payload.c` and `tests/test_keys.c`. The parser tests cover the
asymmetry the rules require — a required field missing rejects the record
whole, a malformed optional is merely absent — which here means a mode switch
carrying one unreadable knob is still a mode switch, and a relative or
oversized `path` costs the caller its default and never the capture.

**Negative-tested**, three planted faults, each caught by the test that exists
for it:

| planted | caught by |
|---|---|
| `mode_ok()` accepts a leading non-lowercase character | "mode starts with a letter" (+2 more) |
| `settings_truncated` never set | "and says it was truncated" |
| an oversize `path` truncated instead of dropped | "an oversize path is dropped, never truncated to a real one" |

## Live acceptance

Not a soak — the trigger was sitting right there, so it was fired in the
session that wrote the contract. Run **from kai**, which is the host `just
pub` and `just dump` run on and therefore the host whose reachability is the
one that matters:

```
kdash-pub set kdash:panelmode:kai '{"mode":"golz","settings":{"density":"0.35","speed_ms":"40"}}'
kdash-pub set kdash:panelshot:kai '{"path":"/var/tmp/kdash-014-check.bmp"}'
KDASH_PANEL_HOST=kai ./build/kdash_dump
```

```
== panel control (kdash:panel|panelmode|panelshot:kai) ==
  panel:     no command
  panelmode: mode=golz  2 setting(s)  issued 2s ago  [a booting panel would act on this]
               density = 0.35
               speed_ms = 40
  panelshot: path=/var/tmp/kdash-014-check.bmp  issued 2s ago  [a booting panel would act on this]
```

That is the whole path in one line each: publisher namespace check, key
grammar, `ts` stamped by the wrapper, the payload against its schema, the typed
reader, and the edge rule saying a booting panel would act. `panel: no command`
is the third family answering `KDASH_ABSENT` — the untouched one, proving the
three are read independently.

`kai` was chosen because nothing consumes kai's panel keys. Both probe keys
were deleted afterwards (`DEL` × 2, verified by `KEYS kdash:panel*:kai`
returning nothing): these families are ts-owned with no TTL, so a probe left
behind would sit on central forever.

## Repaired in passing

Nothing outside the sprint's own scope needed repair; `just check` was green on
`main` before the first edit and the only pre-existing thing touched was the
duplicated key-construction block, folded into `prefixed_host_key()` while
adding the two that would have been its third and fourth copies.

## Follow-ups

None filed. The two consumer-side obligations this contract creates — the pair
host becoming configuration rather than discovery, and refusing a screenshot
path outside a directory the panel owns — belong to kdeskdash slices 2932 and
2933, which are already in the program and already have this page to read.
