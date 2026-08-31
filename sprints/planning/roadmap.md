# Roadmap

> The general plan for this project. Keep it current; detail lives in the
> sprint records.

## Now

- Sprint 001 (korg:1733): data contract v0 — document the current Redis feed
  reality as-is, define the rules new feeds follow (namespace, SET+TTL vs
  stream, schema-per-feed, TTL/staleness, versioning), and record or queue
  the open decisions (shared-feed home, auth model, discovery stems).

## Next

- Sprint 002: the shared C consumer library for the LVGL dashboards — data
  model + freshness, no rendering; aarch64 and x86_64 targets.
- Rust and Python publisher wrappers (khlenv-based endpoint discovery).
- kstudiodash 005 (korg:1728) consumes kdashdata — the live verification
  that contract + library match what a real dashboard needs.

## Later / Ideas

- The `claude:*` → central-Redis move (CD-7): a korg program — gates and
  slices to be scoped; candidate gates are the C consumer lib (002) and the
  publisher wrappers (003) so the move lands on the new plumbing once.
- Opportunistic migration of legacy feeds into the new namespace.
- Redis ACL writer/reader user split.
- Go / C# wrappers if a real consumer appears (deferred, YAGNI).
