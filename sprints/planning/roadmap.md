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

## Now

- The `claude:*` → central-Redis move (CD-7): **program korg:1755**. Slice 1
  (korg:1752) has landed; next is the kdeskdash cutover + reader repoint
  (korg:1753), then close-out — retire old keys, registry flip, claude schemas
  (korg:1754). Wants a dedicated block of time; several moving parts.

## Next

- kstudiodash 005 (korg:1728) consumes kdashdata — the live verification
  that contract + library match what a real dashboard needs.

## Later / Ideas

- Publish the `kdash-pub` wheel to the homelab package store — built by
  `just pub-wheel`, but nothing consumes it yet.
- A khlenv stem for `kvscf:*`, so CD-8's pin stops being implicit. Belongs
  with the cutover slice that has to make it (korg:1753).
- Opportunistic migration of legacy feeds into the new namespace.
- Redis ACL writer/reader user split.
- Go / C# wrappers if a real consumer appears (deferred, YAGNI).
