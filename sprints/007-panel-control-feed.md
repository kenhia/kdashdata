# 007 — The panel-control feed kstudiodash 006 is blocked on

korg: proposal 1817, work item 1816. Unblocks kstudiodash 006 (korg:1729),
which is paused on its branch waiting for this family to exist.

## Goal

kstudiodash 006 wants kdeskdash to be able to say "show the desktop" to the
big panel. #1722 was written assuming that would be free after sprint 005 —
"the dash will already hold a Redis connection, so this is just a
subscription". It is not: the connection is a **libkdash** handle, read-only by
CD-5 with typed readers only, and kstudiodash's own rule (`src/feeds.h`) is
that nothing in that repo opens a connection, knows a key's grammar, or parses
a payload. So the command channel has to be a family and a reader *here*,
exactly as the `claude:*` readers were added upstream in sprint 006 rather
than hand-rolled in the panel.

One family, one schema, one typed reader, one pure edge-detection helper. No
write path, no CD-5 change.

## Premise check at the brief

Every falsifiable claim in #1816 held as written.

- **libkdash is read-only with typed readers only.** `kdash_conn_t` is opaque
  and exposes no command path (`kdash_conn.h`); `kdash_feed.h` had eight typed
  readers and nothing else; `redisCommand` appears in exactly two units.
- **rules.md really does offer the one-shot pattern** (STRING + `GETDEL`) and
  really does say Nudge is "never state" — so the trap the item warns about is
  a live one, not a hypothetical.
- **`kstudiodash:*` cannot serve.** The registry lists it as a *dashboard-local*
  namespace, "reserved — nothing yet", and kstudio runs no local Redis.
- **KDASH_ABSENT / KDASH_UNAVAIL already split absent from unreachable**, which
  is consumer contract #3 satisfied before the sprint started.
- **The publisher half needs no new code — and that is slightly better than
  #1816 predicted.** It said "kdeskdash writes the key; that is its own work
  item". True, but the *wrapper* side is already done: `kdash` is in the
  namespace table both wrappers accept, and `kdash-pub set` stamps `ts` for
  free (`publishers/rust/src/payload.rs`). The kdeskdash work is a button that
  shells one command, not a publisher.

kdashdata is not in the cross-project-planning routing table, so no guiding
plan applies.

## Decisions

### The shape: ts-owned state, not a one-shot command (now CD-17)

Taken as #1816 proposed it, and the reasoning is worth having written down
because the `GETDEL` one-shot pattern genuinely looks like the better fit.

`GETDEL` is a **write**, and libkdash exposes none by design. Using it would
mean carving an exception into CD-5 — the property that makes this library
safe to link into three dashboards — to save a consumer from keeping one
`double`.

The second reason is the one that is easy to miss, and it is why this is not
merely "the pattern that fits the library we happen to have". A command read
level-triggered ("Redis says `desktop`, so be showing the desktop") means that
the moment Ken switches a VT by hand, the panel yanks it straight back, and
keeps doing so forever. Acting on a **change of `ts`** is the same one-shot
discipline `--claim-after` already applies to kstudiodash's boot race, for the
same reason.

### The edge detection lives here, not in the panel

`kdash_panel_actionable()` is the sprint's one judgement call beyond the shape.
"Is this a command I have already acted on" has exactly one right answer, and
a second dashboard deriving it independently is how two panels come to
disagree about what a button press means — the CD-16 argument, applied to a
smaller thing. It is pure, it takes its threshold as a parameter like every
other threshold here, and it composes on the existing `kdash_ts_stale()`
rather than sitting beside it.

Two properties it is worth being explicit about:

- **Strictly newer (`>`), not merely different.** A republish of an unchanged
  command is the same command. A stamp that went backwards cannot resurrect an
  older one.
- **A zeroed record is never actionable.** That is what makes the KDASH_ABSENT
  path safe: a caller that ignores the status and just asks the question still
  behaves correctly.

### `ts` is required *and positive*, which the sibling families do not demand

Everywhere else here `ts` is the record's age. In this family it is the
record's **identity**, because the consumer acts on it advancing. A record
whose stamp cannot be compared is not a weakly-dated command, it is no command
at all — so `kdash_parse_panel` rejects it, and the schema says
`exclusiveMinimum: 0`. This matches the claude HASH helpers' `field_ts`, which
already treats 0 as "no stamp" rather than "the epoch".

### Construction is the only choke point, so there is no `_parse`

Every other family in `kdash_keys.h` is **discovered** — a SCAN or a member set
hands a reader keys it did not choose — which is why parsing is where their
choke point sits. This one is not discovered: a panel builds the key with its
own name on it. So `kdash_panel_key()` ships and no parse does, with the header
saying why, so the next reader does not think it was forgotten. Add the parse
the day something enumerates panels.

The key also shares `KDASH_KEY_MAX` rather than getting a constant of its own
(`kdash:panel:` + a 63-char token is 76 bytes against that 128), and a test
pins that.

### The 60 s window

A command has a shorter useful life than a status card: a panel that was down
for an hour must come back to its own screen, not to whatever it was told while
it was away. 60 s is the same window `services` uses, and it is generous —
a consumer polling on a render tick sees a new command within seconds.

## What shipped

**Contract**
- `contracts/schemas/kdash-panel.schema.json` — the source of truth: `want`
  (closed enum `dash`|`desktop`), `ts` (required, exclusiveMinimum 0).
- `contracts/registry.md` — the family row, the ts-owned reasoning, the reader
  and the one-line writer; the `kstudiodash:*` local row now says *why* the
  control feed is not there; the Reserved section counts two families.
- `docs/architecture.md` — CD-17, including what the decision costs (a command
  whose consumer was down longer than the window is silently dropped, and
  nothing tells the publisher — right for a screen switch, wrong for a command
  with an invisible side effect).

**Pure core** (host-tested, no Redis)
- `kdash_keys.h/.c` — `KDASH_KEY_PANEL_PFX`, `kdash_panel_key()`.
- `kdash_freshness.h` — `KDASH_PANEL_WINDOW_S`.
- `kdash_payload.h/.c` — `kdash_panel_want_t`, `kdash_panel_t`,
  `kdash_parse_panel()`, the two enum-word helpers, `kdash_panel_actionable()`.

**I/O shell** (verified live, not by `just check` — CD-10)
- `kdash_feed.h/.c` — `kdash_panel(c, host, out)`: one GET on an ordinary
  central-stem handle. Not the claude stem; this is a `kdash:*` family.

**Elsewhere**: the panel section in `kdash_dump` (this host's key by default,
`KDASH_PANEL_HOST` for another panel's), the README snippet, and a stale
comment in `kdash.h` corrected — it still called `kdash_payload.h` "the five
schema'd payloads" after sprint 006 took it to eight.

## Negative tests

Five faults planted, all five caught, tree restored green each time.

| Planted fault | Caught by |
|---|---|
| An unrecognised `want` defaults to `dash` instead of rejecting | 3 checks — the typo-sends-the-panel-home bug |
| `kdash_panel_actionable` compares `!=` rather than `>` | 1 check — a republish would re-fire the command |
| `kdash_panel_actionable` drops the staleness window | 1 check — a restart would replay an old switch |
| `ts: 0` accepted as a command | 1 check |
| `kdash_panel_key` skips the token contract | 4 checks, including `kai:evil` and `kstudio/../kai` |

## Live verification

`./build/kdash_dump` against `rpi53:6379` with `REDISCLI_AUTH` from the CD-12
env file, publishing through `kdash-pub` and cleaning up after. Every branch of
the reader, end to end:

```
== panel control (kdash:panel:kai) ==      # nothing published yet
  no command for this panel

$ kdash-pub set kdash:panel:kai '{"want":"desktop"}'
  want=desktop  issued 0s ago  [a booting panel would act on this]

$ kdash-pub set kdash:panel:kai '{"want":"dash","ts":<now-3600>}'
  want=dash  issued 3600s ago  [outside the window — a booting panel would ignore it]

$ kdash-pub set kdash:panel:kai '{"want":"desk"}'     # off-schema enum word
  no command for this panel

== panel control (kdash:panel:kstudio) ==  # the panel this exists for
  no command for this panel
```

Three things that proves beyond "the GET works". The publisher does **not**
police the payload schema — `want: "desk"` published fine — so the reader's
closed enum is the only thing standing between a typo and a panel switching
screens, and it held. The window is applied against a stamp the publisher
preserved rather than restamped. And the test key was deleted afterwards; the
final dump shows the family back to absent.

One documented consequence visible above: a malformed record and an absent key
both render as "no command". That is KDASH_ABSENT's stated meaning in
`kdash_feed.h` ("missing, expired, empty **or malformed**") and it is the same
answer the other seven readers give — a panel needs "act / do not act", and
"there is a broken record here" is a publisher's problem, not a renderer's.

Both architectures build clean with `-Wall -Wextra`: x86_64 natively, aarch64
via `just build-aarch64` against the Pi sysroot. `just check` green.

## Follow-ups

- **kstudiodash 006 resumes** (korg:1729) — a handoff on that proposal carries
  the reader signature, the final payload shape, and the commit to pin.
- **The kdeskdash publisher half is still unfiled.** It is a button that shells
  `kdash-pub set kdash:panel:kstudio '{"want":"…"}'`; korg:1729's notes call for
  a program over the pair, and with this it spans three projects.
- **`kdash_panel_key` has no parse**, deliberately (see Decisions). The day
  something wants to enumerate panels — a kdeskdash view of what it has
  commanded — that is the moment to add it, with a SCAN reader beside it.
