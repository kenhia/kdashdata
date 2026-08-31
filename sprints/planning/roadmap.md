# Roadmap

> The general plan for this project. Keep it current; detail lives in the
> sprint records.

## Now

- Sprint 001 (korg:1733): data contract v0 — document the current Redis feed
  reality as-is, define the rules new feeds follow (namespace, SET+TTL vs
  stream, schema-per-feed, TTL/staleness, versioning), and record or queue
  the open decisions (shared-feed home, auth model, discovery stems).

## Next

- Sprint 002 (korg:1751): the shared C consumer library — data model +
  freshness, no rendering; aarch64 and x86_64. Gates kstudiodash 005
  (korg:1728); deliberately outside the relocation program.
- Sprint 003 (korg:1752): Rust + Python publisher wrappers + khlenv stems —
  slice 1 of the relocation program.
- The `claude:*` → central-Redis move (CD-7): **program korg:1755** —
  003 (korg:1752) → kdeskdash cutover + repoint (korg:1753) → close-out:
  retire old keys, registry flip, claude schemas (korg:1754). Wants a
  dedicated block of time; several moving parts.
- kstudiodash 005 (korg:1728) consumes kdashdata — the live verification
  that contract + library match what a real dashboard needs.

## Later / Ideas

- Opportunistic migration of legacy feeds into the new namespace.
- Redis ACL writer/reader user split.
- Go / C# wrappers if a real consumer appears (deferred, YAGNI).
