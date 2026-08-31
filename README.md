# kdashdata

> **Coming soon** — this is a scaffold; there are no consumable contracts or
> code here yet.

kdashdata owns data movement for my homelab's LVGL dashboards: documented
Redis feed contracts (JSON schema files as the machine-readable source of
truth), a shared C consumer library the dashboards link, and thin publisher
wrappers (Rust and Python). The dashboards themselves — kpidash, kdeskdash,
kstudiodash — live in their own repos and consume this one.

## Planned layout

- `contracts/` — feed contracts: one JSON schema per feed + the contract
  rules (namespaces, freshness, versioning)
- `docs/` — architecture and usage
- the C consumer library and the Rust/Python wrappers arrive in later
  sprints; their directories are created when they carry code, not before

## Development

Uses the [kprojects](https://github.com/kenhia/kprojects) minimal harness:
`just` lists recipes, `just check` runs the gates.

## License

MIT — see [LICENSE](LICENSE).
