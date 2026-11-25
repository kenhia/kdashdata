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
