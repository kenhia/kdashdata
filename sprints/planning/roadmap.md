# Roadmap

> The general plan for this project. Keep it current; detail lives in the
> sprint records.

## Done

- Sprint 001 (korg:1733): data contract v0 — today's Redis feed reality
  documented as-is, the rules new feeds follow, and the standing decisions
  (CD-1…CD-8).
- Sprint 002 (korg:1751): the shared C consumer library (`libkdash`) — data
  model + freshness, no rendering; aarch64 and x86_64. Gates kstudiodash 005
  (korg:1728); deliberately outside the relocation program.
- Sprint 003 (korg:1752): the publisher wrappers (Rust crate + CLI, Python
  wheel), the `KDASH_CLAUDE_REDIS` stem, and CD-12's hook-context auth route —
  slice 1 of the relocation program.
- Sprint 004 (korg:1756): `kdash-pub` distribution — the package store, the
  fixed absolute path, and the three publisher hosts (CD-13).
- Sprint 005 (korg:1754): relocation close-out — `claude:*` live on the
  central Redis, the old home retired, registry flip, claude schemas. **CD-7
  is done**; program korg:1755 closed.
- Sprint 006 (korg:1784): the `claude:*` C readers libkdash was missing — key
  grammar, the HASH field/value parser shape (CD-15), the derived display model
  (CD-16), a stem-parameterized connection handle, and the CMake options that
  let a dashboard consume this repo as a submodule. Slice 1 of program
  korg:1785.

## Now

- kstudiodash 005 (korg:1728) consumes kdashdata — slice 2 of program
  korg:1785, and the live verification that contract + library match what a
  real dashboard needs. Unblocked by sprint 006.

## Next

- kdeskdash adopts these readers (kdeskdash korg:1783), retiring the duplicate
  claude logic sprint 006 knowingly left in place. Not urgent: migrating a
  panel people look at daily is a different risk from adding a reader.

## Later / Ideas

- Publish the `kdash-pub` wheel to the homelab package store — built by
  `just pub-wheel`, but nothing consumes it yet.
- A khlenv stem for `kvscf:*`, so CD-8's pin stops being implicit. Belongs
  with the cutover slice that has to make it (korg:1753).
- Opportunistic migration of legacy feeds into the new namespace.
- Redis ACL writer/reader user split.
- Go / C# wrappers if a real consumer appears (deferred, YAGNI).
