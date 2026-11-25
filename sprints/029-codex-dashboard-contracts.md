# Sprint 029 — Codex dashboard contracts

Proposal korg:4945, covering WI 4936 in program korg:4951.
Branch `029-codex-dashboard-contracts`.

## Premise check

The premise holds. The registry has no Codex limits hash and the v1 Agents
schema has no provider field. Its bar objects already permit extra fields.
The local ksitrep enum has Claude, Ghcp and Other. korg:4946 adds Codex.
No guiding plan lists kdashdata.

## Decisions

CD-33 specifies a central Redis hash keyed by window duration. The Pro fixture
puts the 10080-minute reading in the seven-day fields and explicitly clears
the five-hour fields. CD-31's addendum specifies provider labels and the
fallback readers use when the provider is unavailable or unrecognized.

The kdash-pub namespace allowlists include the new family. The existing C
Agents reader is exercised with both provider-tagged and legacy bars.

## Deployed

On 2026-11-24 PST, after PR #30 squash-merged as `6381ec3`, `just publish`
built and published `kdash-pub 0.1.0-6381ec3` for Linux x86_64, Windows
x86_64 and macOS arm64. The store points `latest` at that version.

`just deploy` verified the artifact checksums and confirmed the installed
version on kai, kubs0, cleo, kimac and thrall (Knarr deploy
`01a21067-57d8-7cbf-903a-809492348aee`). Knarr skipped the optional hosts
komarchy and kbook2 after SSH timeouts and thrall-arch after a DNS failure.

On kai, the installed binary successfully read `codex:limits seven_day_pct`
from a local Redis test socket and returned the expected `24.5`.
