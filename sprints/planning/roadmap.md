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
- Sprint 007 (korg:1817): the panel-control feed — `kdash:panel:<host>`, the
  first feed that tells a dashboard to *do* something, and CD-17 on why a
  control feed is ts-owned state rather than a `GETDEL` one-shot. Unblocks
  kstudiodash 006.
- Sprint 008 (korg:1915): `kdash:stale:<host>:<deployer>` — the feed for hosts
  that are legitimately unreachable, and CD-18 on a presence-owned flag where
  absence is the only all-clear.
- Sprint 009 (korg:2217): the shared apartment-temperature bands, so a panel
  stops porting thresholds by hand, plus the counted-reader contract (a partial
  list is indistinguishable from a complete one, so these readers return -1).
- Sprint 010 (korg:2427): the publishers join the fleet's per-host secrets file
  ahead of the per-user files CD-12 shipped with (CD-19) — one slice of the
  simplify-secrets program korg:2440.
- Sprint 011 (korg:2679): komarchy — the laptop, in no deploy target and ten
  days behind — becomes one and gets the CD-19 build.
- Sprint 012 (korg:2747): the contracts the kxeneon Agents panel was blocked
  on, code-free — `kdash:agentact`, a process monitor's verdict on an agent and
  the first family keyed deliberately to join `claude:session` (CD-20), and
  `ghcp:session` for Copilot CLI, outside `kdash:` by named exception and
  mirroring `claude:session` field-for-field (CD-21). Slice 3 of korg:2751.
- Sprint 013 (korg:2788): 012 legalised `ghcp` in prose and taught neither
  publisher allowlist, so every `ghcp:*` write was refused while the contract
  called it legal. Both sides taught, plus the gate that compares them — the
  one check neither per-language gate could ever make.
- Sprint 014 (korg:2931): the contract slice opening the Redis-consolidation
  program (korg:2935, five servers to two) — CD-8 amended so the dev pair's
  `kvscf:*` exchange lives on central, and the panel-control family grown two
  siblings, one per verb (CD-22). CD-23 records what that makes the fleet
  password.
- Sprint 015 (korg:2978): what the contracts say, now enforced — every schema
  carries `examples` it must accept and `x-counterexamples` it must reject
  (CD-24); `kdash-pub` grew `get` and `scan` in both wrappers with absence as a
  distinct exit code (CD-14 amended); and the repo declares its deploy behind a
  self-skipping `publish` versioned from the binary's own inputs, so a
  contract-only sprint no longer churns four hosts.

## Now

- The rest of the Redis-consolidation program (korg:2935), which is kdeskdash's
  and k-homelab's to carry: fold rpidash2's pair Redis into central
  (korg:2932), then panel state to a file with commands from central
  (korg:2933), then retire the three servers (korg:2934). Nothing in this repo
  blocks it — sprint 014 landed the contract half.

## Next

- kdeskdash's Copilot publisher (korg:2755), unblocked by sprint 013, and the
  k-homelab recipe behind it (korg:2756).
- kdeskdash adopts the `claude:*` readers (korg:2218, work item 1783),
  retiring the duplicate claude logic sprint 006 knowingly left in place. Not
  urgent: migrating a panel people look at daily is a different risk from
  adding a reader.
- kstudiodash's first consumption of the library (korg:1728) — still the live
  verification that contract and library match what a real dashboard needs.

## Later / Ideas

- Publish the `kdash-pub` wheel to the homelab package store — built by
  `just pub-wheel`, but nothing consumes it yet.
- A khlenv stem for `kvscf:*`, so CD-8's pin stops being implicit. Belongs
  with the cutover slice that has to make it (korg:1753).
- Opportunistic migration of legacy feeds into the new namespace.
- Redis ACL writer/reader user split.
- Go / C# wrappers if a real consumer appears (deferred, YAGNI).
