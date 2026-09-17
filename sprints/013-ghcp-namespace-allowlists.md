# Sprint 013 — kdash-pub learns the `ghcp` namespace it legalised

korg: proposal 2788, work item 2781. Slice 3.5 of program korg:2751 ("kxeneon
Agents panel"), inserted mid-program by the overseer after kdeskdash 038's live
acceptance ran into it. Run as an overseen karc leg on kai.

**Goal.** Sprint 012 legalised the `ghcp` family — `ghcp-session.schema.json`,
the `Family: ghcp` registry section, the rules.md namespace exception, CD-21 —
and taught neither publisher wrapper the namespace. Every `ghcp:*` write was
refused as off-contract while the contract said it was legal. Teach both
allowlists, and add the gate that makes the class of fault impossible rather
than just this instance of it.

## Premise check

WI 2781's claims, re-measured on kai before anything changed.

| claim | verdict |
|---|---|
| `publishers/rust/src/keys.rs` `NAMESPACES` stops at `kstudiodash` | **holds** — six entries, no `ghcp` |
| `publishers/python/src/kdash_pub/keys.py` `NAMESPACES` stops at `kstudiodash` | **holds** — identical six |
| deployed binary on kai is `0.1.0-83e5795 (2026-09-12)` | **holds** |
| the schema merged as `5203e29` | **holds** — sprint 012, on main |
| `ghcp:` refused / `claude:` accepted, same batch, stem and host | **holds** — reproduced below before any edit |

Not a stale binary. No source knew the namespace, so no deploy could have
fixed it.

## What shipped

### The two-line fix

`ghcp` added to both `NAMESPACES` arrays, positioned with the central families
rather than the dashboard-local pair. Strictly permissive: it cannot break an
existing feed.

Both doc comments described the array by **counting** its groups — "the next
three are grandfathered; the last two are dashboard-local" — which stops being
true the moment anything is inserted. They now name the groups instead. That is
the same rot as the one this sprint is fixing, one level down.

`ghcp:session:kai:abc-123` joined the governed-key table both suites pin, so
the accepted-keys list in Rust and Python still agree with each other.

### The gate — `scripts/check.py` holds the registry and the code together

Every family `contracts/registry.md` documents must appear in **both**
allowlists, and every namespace either allowlist accepts must be documented.
Both directions, because the CLI's own error message states the second one:
*"a feed with no schema in kdashdata is off-contract"*.

`check-docs` is the only gate that can do this. `check-rust` and `check-python`
each test their own side against itself, so both were green for the whole time
`ghcp` was documented and refused — the contract and the enforcement were never
compared by anything. This is the first time the docs gate reaches into
`publishers/`, and that is the reason.

Two sources feed the documented set:

- `## Family: <name> (…)` headings, **excluding `retired` ones**. The
  registry's own vocabulary is `live` / `migrating` / `retired`; requiring the
  publishers to keep accepting a retired family would be the gate enforcing the
  opposite of the contract.
- the `## Dashboard-local namespaces` table. `kstudiodash:*` is documented as
  holding nothing at all and is still in both allowlists, so "not
  schema-governed" plainly does not mean "not publishable".

The parse is bounded to that one section deliberately: the same table-row shape
appears in `Homes` and in `Reserved`, and neither is a namespace declaration.

**Both parsers fail loudly when they stop matching.** A renamed array or a
reshaped heading makes this check find nothing, and a check that finds nothing
passes — the exact shape of an empty result being indistinguishable from a
suppressed failure. So an unparseable allowlist and an empty family set are
each errors in their own right, not no-ops, and negative tests 5–7 below are
the proof.

### Both sides of the contract, in prose

`contracts/rules.md`'s Namespaces section now says adding a family is two
edits, that `just check` insists on both, and what happened when it did not.

## The third allowlist that does not exist

Checked rather than assumed: the C consumer library has no namespace list. It
validates key *grammar* — charset, segments, lengths — and never the family
set. There are exactly two allowlists in this repo and the gate covers both.

## Negative tests

The gate was planted with each fault it exists to catch and watched to exit 1.
All seven, run against the real tree and reverted:

| # | planted fault | result |
|---|---|---|
| 1 | `ghcp` removed from the Rust allowlist (the actual bug) | rc 1, names `ghcp` and the Rust file |
| 2 | `ghcp` removed from the Python allowlist | rc 1, names `ghcp` and the Python file |
| 3 | `kbogus` added to the Rust allowlist, undocumented | rc 1, "register it or drop it" |
| 4 | a new family documented in the registry, taught to neither side | rc 1, **two** errors, one per publisher |
| 5 | the Rust array renamed `ALLOWED_NS` | rc 1, "went blind rather than failing" |
| 6 | `## Family:` heading format changed | rc 1, names the parser, not a silent pass |
| 7 | the dashboard-local section renamed | rc 1, "cannot tell a missing section from an empty one" |

4 is the one that matters most: it is this sprint's own history replayed, and
it fails on both sides at once.

## Verified live — against the central Redis, from kai

Every run from kai, the host that publishes, with the same stem
(`KDASH_CLAUDE_REDIS`), the same batch and the same host token. Only the
namespace and the binary vary.

| # | binary | key | rc |
|---|---|---|---|
| A | deployed `0.1.0-83e5795` | `ghcp:session:kai:probe` | **1** — refused, off-contract |
| B | deployed `0.1.0-83e5795` | `claude:session:kai:probe` | **0** — accepted |
| C | this branch's build | `ghcp:session:kai:probe` | **0** — accepted |

B is the control that makes A a fact about the allowlist rather than about
endpoint resolution, auth or the transport — all three of which it proves are
fine. C is the fix.

The write in C landed for real: both fields read back (`host=kai`,
`ts=1789626015`), so the whole batch applied, not just the key check. Read back
through the **old** deployed binary it is refused the same way — `hget` runs the
same check — which is a second, independent sighting of the same allowlist.

Both probe keys were deleted and the deletions confirmed by a follow-up read
returning empty.

## Why the deploy is not in this turn

The proposal's acceptance asks for the redeploy in-session, and it is coming —
on the ship turn, not this one, and the ordering is forced rather than chosen.

`just publish` refuses a dirty tree and refuses to move the `latest` pointer
from a branch, because a branch commit vanishes at squash-merge and the fleet
must never resolve `latest` to a commit that no longer exists. `just deploy`
installs `latest`. So a binary-changing sprint publishes and deploys **from
main, after the merge** — which is exactly what sprint 010 did (`83e5795`
merged, then deployed, then `9f1fc82` recorded it).

Sprint 011 deployed before its ship, and said why it could: that branch changed
no binary. This one changes the binary, so that exception does not apply.

The code is fully accepted in-session regardless — run C above is the deployed
artifact's content, proven against the live Redis from the host that publishes.
What remains is distribution, and it is the ship turn's first action.

## Repaired in passing

- **Both allowlist doc comments counted their groups** ("the next three… the
  last two"), which the insertion falsified. Rewritten to name the groups.
  Unambiguous, mechanical, and in the file the sprint was already editing.

- **The CLAUDE.md Status check was case-sensitive.** It refused "Sprint 013
  taught both sides…" — a sentence that opens correctly — and would have been
  satisfied by lowercasing a sentence-initial word. A gate that asks for a
  grammatical error to pass a string compare teaches people to work around it.
  Now compared case-insensitively, and re-negative-tested afterwards: a Status
  paragraph naming sprint 011 still exits 1.

## Deployed

`kdash-pub` **0.1.0-8d13cce** to all four publisher hosts, 2026-09-16, from kai,
off merged `main` (`8d13cce`).

**`just publish`** — linux and windows binaries built from one checkout, one
`--version` label, one store directory. `latest -> 0.1.0-8d13cce`. The pointer
moved because this ran from `main`; from the branch it would have carried
`--no-latest`, which is why the deploy waited for the merge.

**`just deploy-all`** — `ok: true`, every host. komarchy answered its
reachability probe (run from kai, the host doing the deploying) and took the
build in the same run rather than being skipped; sha256 `8ff48516…`, backup
rotated to `kdash-pub.prev`, restart and readiness `skipped` as always — it is
a hook-invoked CLI with no unit.

**Stamps confirmed by naming the hosts**, not by iterating what the deploy
reported — the rule `deploy-all`'s own comment exists to enforce, after kpolice
sprint 002 left cleo on a commit that no longer existed:

| host | path | stamp |
|---|---|---|
| kai | `/usr/local/bin/kdash-pub` | `0.1.0-8d13cce (2026-09-16)` |
| kubs0 | `/usr/local/bin/kdash-pub` | `0.1.0-8d13cce (2026-09-16)` |
| komarchy | `/usr/local/bin/kdash-pub` | `0.1.0-8d13cce (2026-09-16)` |
| cleo | `C:\tools\bin\kdash-pub.exe` | `0.1.0-8d13cce (2026-09-16)` |

One version across the fleet, which was the whole point of the overseer's
ruling to redeploy in one go rather than following the slices.

### Verified live, post-deploy

The acceptance run, with the **deployed** binary this time — the same batch that
returned rc 1 before the ship:

| host | probed from | write | read back | delete |
|---|---|---|---|---|
| kai | kai | **rc 0** | `host=kai` | gone, confirmed by re-read |
| kubs0 | kubs0 | **rc 0** | `host=kubs0` | gone, confirmed by re-read |

Each probe ran **on** the host it is a fact about. Both keys deleted and the
deletions confirmed by a follow-up read returning empty.

cleo's copy is the overseer's to verify from cleo, by the same method — a
`ghcp:session:cleo:probe` write and delete. komarchy was deployed and its stamp
confirmed, but not feed-tested: it runs no Copilot hooks, so there is nothing
there for the feed to carry.
