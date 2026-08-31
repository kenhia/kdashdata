# kdashdata

> **Early days** — contract v0 (docs + schemas) is here; the consumer
> library and publisher wrappers are not yet.

kdashdata owns data movement for my homelab's LVGL dashboards: documented
Redis feed contracts (JSON schema files as the machine-readable source of
truth), a shared C consumer library the dashboards link, and thin publisher
wrappers (Rust and Python). The dashboards themselves — kpidash, kdeskdash,
kstudiodash — live in their own repos and consume this one.

## Layout

- [`contracts/rules.md`](contracts/rules.md) — the rules new feeds follow
- [`contracts/registry.md`](contracts/registry.md) — the feed inventory
- [`contracts/schemas/`](contracts/schemas/) — one JSON Schema per feed payload
- [`docs/architecture.md`](docs/architecture.md) — topology and standing decisions
- the C consumer library and the Rust/Python wrappers arrive in later
  sprints; their directories are created when they carry code, not before

## Development

Uses the [kprojects](https://github.com/kenhia/kprojects) minimal harness:
`just` lists recipes, `just check` runs the gates.

## License

MIT — see [LICENSE](LICENSE).
