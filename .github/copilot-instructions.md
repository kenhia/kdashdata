<!-- kproject:begin — managed by kprojects; do not edit inside this block -->
## kproject conventions

This project uses the kproject minimal harness
(<https://github.com/kenhia/kprojects>). Keep context small; prefer doing
over ceremony.

### Layout

- `sprints/` — the project's evolution, one record per PR-sized unit of
  work (a "sprint")
  - `planning/` — planning docs; at minimum `roadmap.md` (the general plan)
  - `review/` — more formal reviews as the project matures
  - sprint records: `###-<short-name>.md` for small projects, or a
    `###-<short-name>/` directory of files for larger/more formal ones
  - a sprint record is one informal narrative: goal, decisions, what
    shipped, follow-ups — written during the sprint, not after
  - projects that deploy end the record with a `## Deployed` section:
    what shipped, where, when, and what was verified live — appended
    after the deploy, not predicted before it
- `docs/` — project documentation, architecture, usage
- `.scratch/` — git-ignored scratch space for user or agent ephemera;
  use it instead of /tmp
- `justfile` — dev recipes; default recipe is `@just --list`; `just check`
  runs the CI gates; `just deploy` (or variants) if the project deploys
- `.env` — git-ignored; tokens and environment vars

### Workflow

- One sprint ≈ one PR. Sprint proposals and work items are managed in
  `korg`; durable cross-project knowledge goes in `klams`.
- Mark each work item resolved as its work completes — don't batch the
  resolutions into sprint-ship. A proposal's progress should be readable
  while the sprint is running, which is the only time it is useful.
- If the korg or klams MCP tools are unavailable in your session, say so
  up front — don't silently work around missing infrastructure.
- A few projects share contract surfaces with siblings and have a
  **guiding plan** constraining how those change; most have none, and one
  grep is the whole cost of finding out. Grep the `index.md` routing
  table in `kai:~/src/tools/cross-project-planning` — a local path on
  kai, read through kaed from any other host (`root: "kai:src"`, path
  `tools/cross-project-planning/…`); don't clone a second copy. Not
  listed → nothing applies. Listed → read the mapped plan folder before
  planning sessions and before changing a contract surface it names, and
  amend the plan in the same ship when what you build diverges from it.
- TDD preferred: write the failing test first when practical.

### Tooling preferences

- No stack the harness could name, so `just check` is yours to write. Ask
  what this repo can actually get wrong — a documents repo's failure mode is
  a stale cross-reference, not a type error
- Add no dependency to make a gate: a stdlib script or a shell one-liner
  keeps a repo that had no dependencies still having none
- Skip what isn't yours to verify — external URLs, machine-local paths
- **Negative-test it.** Plant the error the gate exists to catch and watch it
  exit 1. A gate never seen to fail is not a gate, and the seeded placeholder
  fails on purpose until you replace it
- License is MIT unless specifically directed otherwise
<!-- kproject:end -->

## Project

kdashdata owns data movement for the homelab's LVGL dashboards — kpidash,
kdeskdash, kstudiodash. Three deliverables, in priority order: (1) the
documented Redis feed contracts (JSON schema files are the machine-readable
source of truth; prose docs stay consistent with them); (2) a shared C
consumer library the dashboards link (data model + freshness only, no
rendering; aarch64 and x86_64); (3) thin publisher wrappers, Rust and Python.
The dashboards and their publisher daemons live in their own repos and
consume this one — nothing here runs as a service.

Status: fresh scaffold. Contract v0 is sprint 001 (korg:1733).

Conventions and constraints:

- Shared/cross-dashboard data goes to the central Redis; dashboard-specific
  data goes to a Redis local to that dashboard's host. No distributed Redis.
- No big-bang key renames: contract v0 documents today's keys as-is; new
  feeds land under the new namespace; old feeds migrate opportunistically.
- Endpoint discovery is khlenv's job — contracts define stem variables,
  never hardcoded endpoints.
- Language wrappers beyond Rust/Python are deliberately deferred until a
  real consumer exists.
- korg project: kdashdata. Read first: `sprints/planning/roadmap.md`, then
  `docs/` and `contracts/` once sprint 001 lands.
- `just check` is a python3-stdlib gate (`scripts/check.py`): every JSON
  file parses, every relative markdown link resolves.
