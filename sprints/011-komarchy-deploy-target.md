# Sprint 011 — komarchy becomes a deploy target, and gets the CD-19 build

korg: proposal 2679, work item 2678. One slice of program korg:2440
("simplify homelab secrets"), rank 19.96 — inserted by the overseer after
korg:2675 and before the proof pass korg:2439, to take a trap off that pass.

**Goal.** komarchy was running `kdash-pub 0.1.0-7fe2c87` — a build with no
per-host-secrets rung — because it was in no deploy target at all. Put it in
one, deploy `83e5795`, and prove the CD-19 rung answers there under the user
manager. That unblocks soak WI 2676 and stops the program's `REDISCLI_AUTH`
rotation leaving the laptop's hook publishing silently sending the old value.

## Premise check

Every claim in WI 2678 was re-measured on 2026-09-15 before any change, each
from the host that would do the work.

| claim | verdict |
|---|---|
| komarchy runs `0.1.0-7fe2c87` (2026-09-01), installed Sep 2 16:56 as root | **holds** — byte-for-byte, mtime included |
| `strings` finds no `khomelab` and no `auth from` in that binary | **holds** — 0 and 0 |
| kai and kubs0 run `0.1.0-83e5795` (2026-09-12) | **holds** |
| `just deploy` targets `kai,kubs0`; `deploy-cleo` targets cleo; komarchy is in no target | **holds** |
| komarchy is reachable now (Ken rebooted it and is at the desktop) | **holds** — `ssh komarchy true` rc 0, clean stderr, probed **from kai**, the host that runs the deploy |

The probe host is not a detail. A reachability check run from anywhere else
measures *that* machine's route rather than komarchy's availability, and the
two are different facts — program korg:1919 lost a leg to exactly that
confusion.

## The question this slice had to settle first

The proposal's scope opened with "the knarr host list `kai,kubs0` gains
`komarchy`", conditional on knarr having somewhere to put an intermittent
host. It does not, and the measurement is the whole decision.

`knarr deploy kdash-pub --host kai,no-such-host-on-this-tailnet --dry-run`,
from kai, 2026-09-15:

- **exit 3**, aggregate `ok: false`
- `kai` → `ok: true`, every step reported
- the absent host → `ok: false`, `backup failed: checking
  /usr/local/bin/kdash-pub: exit status 255`, later steps
  `skipped: earlier step failed`

Two of the three things the proposal asked for are already true, and knarr is
right about both: the reachable hosts keep their upgrade (a fleet deploy is
not a transaction — `internal/deploy/fleet.go:78-80`), and the absent host is
**named** rather than silently dropped. The third is missing. There is no way
to say *this host may legitimately be absent*, so absence and breakage are the
same outcome.

komarchy's resting state is lid closed. In a shared host list that would make
the everyday `just deploy` exit 3 on most days for a reason that is not a
problem — and a deploy command whose exit code is routinely wrong is one whose
exit code stops being read.

**Filed, not repaired: korg 2680** (project knarr). It is a change to knarr's
CLI *and* its status-document contract — the flag spelling, whether an excused
absence reuses `skipped` or needs a new host-level status, what exit code a
"reported but not severe" outcome gets in a worst-of-by-severity scheme, and
whether honest absence detection needs a probe step ahead of the plan. None of
those is settled by the evidence in front of a calling repo, so this sprint
did not reach into knarr. The item names all four.

## What shipped

**`just deploy-komarchy`** — `knarr deploy kdash-pub --host komarchy`, the
explicit `--host komarchy` step the proposal specified as the fallback.
Failure here is a plain failure on purpose: asking for komarchy by name means
you believe it is awake.

**`just deploy-all` names komarchy**, and runs it last behind an
`ssh -o BatchMode=yes -o ConnectTimeout=5` probe. The probe is the part that
does the work, and it is there to keep **asleep** and **broken** apart:

- probe answers → the deploy runs, and any failure it hits is fatal under
  `set -euo pipefail`, exactly like kai's. An excused absence must not become
  an excused breakage.
- probe silent → the skip is **printed**, twice, naming the recipe to run with
  the lid open. A host quietly missing from a fleet deploy is the one outcome
  `deploy-all` exists to prevent — it is why the recipe exists at all, after
  kpolice sprint 002 left cleo on a commit that no longer existed.

That is a recipe standing in for a knarr feature, the same shape as
`scripts/install-cleo.ps1`, and the comment says so and cites korg 2680 as
what retires it.

**CD-13 gains komarchy** — the install-path table (a fourth row, same path and
same installer as kai and kubs0), and a subsection recording why it is not in
`deploy`'s list, what was measured, and what has to land in knarr before it
can be.

**`publishers/README.md`** names four hosts in the command block and in the
"verify by naming the hosts" rule, and now says to run the per-host check
*through a user manager* rather than from a login shell — a login shell's
groups and a user manager's groups are different facts, and the per-host
secrets file is readable by group.

## Negative tests

Both branches of the new probe were watched, rather than assumed:

| branch | stimulus | result |
|---|---|---|
| skip | probe against an unreachable name | both SKIPPED lines printed, script continued, **rc 0** |
| deploy | probe against komarchy, awake | answers → would deploy |

The skip branch returning 0 is the assertion that matters: it is what makes an
absent komarchy not a failed `deploy-all`.

## Deployed

`kdash-pub` **0.1.0-83e5795** to komarchy, 2026-09-15, from kai.

This deploy ran *before* the ship rather than after, and that is safe here
because the branch changes no binary: `83e5795` was published to the store on
2026-09-12 (sprint 010) and is what `latest` already resolves to. The sprint
ships recipes and docs; the laptop was simply ten days behind the fleet, and
the lid was open.

`just deploy-komarchy` → `knarr deploy kdash-pub --host komarchy`, aggregate
`ok: true`, exit 0:

| step | result |
|---|---|
| stage | ok — uploaded to `/tmp/knarr-kdash-pub-0.1.0-83e5795` |
| backup | ok — rotated to `/usr/local/bin/kdash-pub.prev` |
| install | ok — `/usr/local/bin/kdash-pub`, mode 0755 |
| restart / ready | skipped — no unit, no probe; `kdash-pub` is a hook-invoked CLI |
| confirm | ok — `kdash-pub 0.1.0-83e5795 (2026-09-12)` |

SHA256 `5aeb5147…`, resolved and verified once before the host was touched.

### Verified live on komarchy

The pass is paired with the wrong-password control, which is this program's
standing rule for the changeover — `kdash-pub endpoint` exits 0 even with no
credential at all (WI 2492), so an unpaired exit 0 proves nothing.

Run **under komarchy's systemd user manager**, not a login shell, because that
is the context the hooks actually publish from:

| check | result |
|---|---|
| binary after install | `0.1.0-83e5795`; `khomelab` ×5, `auth from` ×1 — the rungs the old build did not have |
| user manager (WI 2676 criterion 1) | pid 1087, `Groups: 998 1000 1001`; `khomelab` gid is **1001** — carried |
| per-host file | `/etc/khomelab/secrets.env`, `root:khomelab 0640`, carries a `REDISCLI_AUTH` key |
| **pass** — per-user file moved aside, `systemd-run --user --collect --wait --pipe kdash-pub --app kdashdata endpoint` | **rc 0**, `kdash-pub: auth from per-host secrets file (/etc/khomelab/secrets.env)` |
| **control** — the same call with `--setenv=REDISCLI_AUTH=wrong-on-purpose` | **rc 2**, `redis: Password authentication failed- AuthenticationFailed` |

Every exit status was read from the command itself with its output redirected
to a file, never through a pipe — a `cmd | tail` reports the filter's status,
not the command's.

Nothing printed a password. The endpoint (`rpi53:6379`) is an address, not a
credential.

**The per-user file was restored, not deleted**, and the restoration was
verified by a separate call after the script's own trap claimed it: present,
`0600 ken:ken`, 70 bytes, mtime unchanged at Sep 5 22:45, no `.aside-*`
leftovers. Deleting it is soak WI 2676's third criterion and belongs to the
overseer; a leg never closes a soak.

## What this leaves for the overseer

Soak **WI 2676**'s criteria 1 and 2 are now both met and measured above. Only
its step 3 remains — deleting `~/.config/kdash/redis-auth.env` on komarchy
with the read-by-nothing proof. `check_after` is 2026-09-16 and
`invalidated_if` is untouched by this sprint.

The trap recorded on the proof pass korg:2439 — rotating `REDISCLI_AUTH` while
komarchy runs a build with no per-host rung — is gone. komarchy now resolves
from `/etc/khomelab/secrets.env` like the rest of the fleet.

## Repaired in passing

Nothing. The sprint turned up no defect outside its own scope — the one thing
it found that it did not fix is knarr's missing allowed-absent host, and that
is korg 2680 because it needs decisions in knarr's contract, not because it
was inconvenient.
