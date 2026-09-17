#!/usr/bin/env python3
"""Repo gate: JSON parses, markdown links resolve, every schema is
registered, the registry's families match both publishers' allowlists, every
PowerShell script is pure ASCII, and CLAUDE.md's Status line names the newest
sprint record.

Stdlib only, by design — this repo carries contracts and docs, and its
failure modes are a schema that doesn't parse, a stale cross-reference, a
feed whose schema landed without anyone telling the registry about it,
(since sprint 004) a non-ASCII byte in the deploy script cleo runs,
(since sprint 009) an orientation file that has quietly stopped describing
the repo, and (since sprint 013) a family legalised in prose that the code
still refuses.

That last one is the reason this gate reaches into `publishers/` at all, and
it is worth being explicit about: the per-language gates cannot catch it.
`check-rust` and `check-python` each test their own side against themselves,
so both were green the whole time `ghcp:*` was documented and refused. Only a
check that reads the registry and both arrays can compare them, and this is
the only gate positioned to do it.

The code gates live elsewhere: `just check-python`, `just check-rust`, and the
CMake build plus ctest.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
#: Build output and virtualenvs, which carry JSON of their own and are nobody's
#: contract. `target` and `.venv*` arrived with the publisher wrappers; without
#: them this gate reads cargo's fingerprint files and a site-packages tree.
SKIP_DIRS = {".git", ".scratch", "build", "build-aarch64", "target", "dist", "__pycache__"}
SKIP_PREFIXES = (".venv",)
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
SCHEMA_DIR = ROOT / "contracts" / "schemas"
REGISTRY = ROOT / "contracts" / "registry.md"
RUST_KEYS = ROOT / "publishers" / "rust" / "src" / "keys.rs"
PYTHON_KEYS = ROOT / "publishers" / "python" / "src" / "kdash_pub" / "keys.py"
#: `## Family: ghcp (central, live)` — name, then the parenthetical carrying
#: the family's status (`live`, `migrating` or `retired`, per registry.md).
FAMILY_RE = re.compile(r"^## Family:\s*(\S+)\s*\((.*)\)\s*$", re.MULTILINE)
#: The dashboard-local table's first column: `| `kdeskdash:*` | each ... |`.
LOCAL_NS_RE = re.compile(r"^\|\s*`([A-Za-z0-9_-]+):\*`\s*\|", re.MULTILINE)
#: Both allowlists, each a flat list of string literals.
RUST_NS_RE = re.compile(r"pub const NAMESPACES: &\[&str\] = &\[(.*?)\];", re.DOTALL)
PYTHON_NS_RE = re.compile(r"^NAMESPACES = \((.*?)\)", re.DOTALL | re.MULTILINE)
STRING_RE = re.compile(r'"([^"]*)"')
SPRINTS = ROOT / "sprints"
CLAUDE_MD = ROOT / "CLAUDE.md"
#: `007-panel-control-feed.md`, or a `007-panel-control-feed/` directory — the
#: harness allows either spelling for a sprint record.
SPRINT_RE = re.compile(r"^(\d{3})-")
#: Tab, newline, and printable ASCII. Anything else is a parse failure on
#: cleo — see the check below.
ASCII_OK = {0x09, 0x0A, 0x0D} | set(range(0x20, 0x7F))


def skipped(rel: Path) -> bool:
    return any(
        part in SKIP_DIRS or part.startswith(SKIP_PREFIXES) for part in rel.parts
    )


def repo_files(suffix: str):
    for path in sorted(ROOT.rglob(f"*{suffix}")):
        rel = path.relative_to(ROOT)
        if not skipped(rel):
            yield path


def documented_namespaces(registry_text: str, errors: list) -> set:
    """Every namespace contracts/registry.md says exists.

    Two sources, because the registry describes two kinds. Schema-governed
    families get a `## Family:` heading; dashboard-local state is listed for
    visibility in its own table and never gets one. Both are legal to publish
    to — `kstudiodash:*` is documented as holding nothing at all and is still
    in both allowlists — so both belong in the comparison.

    `retired` families are excluded: the registry's own vocabulary says a
    retired family is no longer written, so requiring the publishers to keep
    accepting it would be this check enforcing the opposite of the contract.
    """
    names = set()
    for name, parenthetical in FAMILY_RE.findall(registry_text):
        if "retired" in parenthetical.lower():
            continue
        names.add(name)
    if not names:
        errors.append(
            "contracts/registry.md: no `## Family:` heading parsed — either "
            "the registry lost its families or this gate's parser no longer "
            "matches it. An empty result here would silently pass every "
            "namespace check below, so it is a failure, not a no-op"
        )

    # Bound to the dashboard-local section: the same row shape appears in the
    # `Homes` table and in the Reserved list, and neither is a namespace
    # declaration.
    section = ""
    for block in registry_text.split("\n## "):
        if block.startswith("Dashboard-local namespaces"):
            section = block
            break
    if not section:
        errors.append(
            "contracts/registry.md: no `## Dashboard-local namespaces` "
            "section — this gate reads the publishable namespace list from "
            "it, and cannot tell a missing section from an empty one"
        )
    names.update(LOCAL_NS_RE.findall(section))
    return names


def declared_namespaces(path: Path, pattern: re.Pattern, errors: list) -> set:
    """The NAMESPACES allowlist a publisher wrapper actually enforces."""
    rel = path.relative_to(ROOT)
    if not path.exists():
        errors.append(f"{rel}: missing — cannot check it against the registry")
        return set()
    match = pattern.search(path.read_text(encoding="utf-8"))
    if match is None:
        errors.append(
            f"{rel}: no NAMESPACES array found. It was renamed, reshaped or "
            "moved, and this gate went blind rather than failing — which is "
            "the failure mode it exists to prevent"
        )
        return set()
    return set(STRING_RE.findall(match.group(1)))


def check_namespaces(registry_text: str, errors: list) -> None:
    """Hold contracts/registry.md and both publisher allowlists to each other.

    Sprint 012 landed `ghcp-session.schema.json`, the `Family: ghcp` registry
    section, the rules.md namespace exception and CD-21 — and nothing taught
    either publisher the namespace, so every `ghcp:*` write was refused with
    "a feed with no schema in kdashdata is off-contract" while the schema sat
    in this repo (WI 2781). Both per-language gates were green throughout.

    Both directions are the contract, and the CLI's own error message states
    the second one: a documented family the code refuses is a feed nobody can
    write, and an accepted namespace the registry never documents is a feed
    with no schema, which rules.md calls off-contract.
    """
    documented = documented_namespaces(registry_text, errors)
    arrays = {
        "publishers/rust/src/keys.rs": declared_namespaces(
            RUST_KEYS, RUST_NS_RE, errors
        ),
        "publishers/python/src/kdash_pub/keys.py": declared_namespaces(
            PYTHON_KEYS, PYTHON_NS_RE, errors
        ),
    }
    for rel, declared in arrays.items():
        if not declared:
            continue  # already reported by declared_namespaces
        for missing in sorted(documented - declared):
            errors.append(
                f"{rel}: registry.md documents the `{missing}` namespace and "
                f"NAMESPACES does not list it — every write to `{missing}:*` "
                "is refused as off-contract while the contract says it is "
                "legal (WI 2781)"
            )
        for extra in sorted(declared - documented):
            errors.append(
                f"{rel}: NAMESPACES accepts `{extra}` and contracts/"
                "registry.md documents no such family — rules.md calls a feed "
                "with no schema here off-contract, so either register it or "
                "drop it"
            )


def main() -> int:
    errors = []

    for path in repo_files(".json"):
        try:
            json.loads(path.read_text(encoding="utf-8"))
        except (ValueError, OSError) as exc:
            errors.append(f"{path.relative_to(ROOT)}: invalid JSON: {exc}")

    for path in repo_files(".md"):
        text = path.read_text(encoding="utf-8")
        for target in LINK_RE.findall(text):
            if target.startswith(("http://", "https://", "mailto:", "#")):
                continue
            local = target.split("#", 1)[0]
            if local and not (path.parent / local).exists():
                errors.append(
                    f"{path.relative_to(ROOT)}: broken link -> {target}"
                )

    # rules.md: "A feed exists when its schema file lands here." A schema the
    # registry never mentions is a feed nobody can find — the same class of
    # fault as a broken link, one level up.
    registry_text = REGISTRY.read_text(encoding="utf-8") if REGISTRY.exists() else ""
    for schema in sorted(SCHEMA_DIR.glob("*.schema.json")):
        if f"schemas/{schema.name}" not in registry_text:
            errors.append(
                f"contracts/registry.md: no link to schemas/{schema.name} — "
                "every schema names a feed the registry must list"
            )

    check_namespaces(registry_text, errors)

    # scripts/install-cleo.ps1 runs under Windows PowerShell 5.1, which reads a
    # BOM-less .ps1 as the system ANSI codepage rather than UTF-8. A UTF-8
    # em-dash then arrives as mojibake, and one sitting inside a double-quoted
    # string terminates that string early — so the file fails to parse with
    # errors pointing dozens of lines away from the actual character. The rest
    # of this repo uses em-dashes freely, which is exactly why an editor (or an
    # agent) will eventually put one here. Cheaper to catch at `just check`
    # than on cleo.
    for path in repo_files(".ps1"):
        raw = path.read_bytes()
        for lineno, line in enumerate(raw.split(b"\n"), start=1):
            bad = sorted({b for b in line if b not in ASCII_OK})
            if bad:
                shown = ", ".join(f"0x{b:02x}" for b in bad)
                errors.append(
                    f"{path.relative_to(ROOT)}:{lineno}: non-ASCII byte(s) {shown} — "
                    "a .ps1 cleo runs must be pure ASCII (see the file's .NOTES)"
                )

    # CLAUDE.md's Status line is the first thing an agent reads in this repo,
    # and it had gone four sprints without being touched (WI 1928) — which is
    # worse than omitting it, because it confidently describes a state the repo
    # left months ago. Nothing about writing that line can be automated, but
    # noticing it is stale is one comparison, and it turns a silent rot into a
    # failing gate in the sprint that caused it.
    newest = max(
        (m.group(1) for p in SPRINTS.iterdir() if (m := SPRINT_RE.match(p.name))),
        default=None,
    )
    if newest is None:
        errors.append("sprints/: no NNN-named sprint record found")
    elif CLAUDE_MD.exists():
        claude_text = CLAUDE_MD.read_text(encoding="utf-8")
        status = next(
            (ln for ln in claude_text.splitlines() if ln.startswith("Status:")),
            None,
        )
        if status is None:
            errors.append("CLAUDE.md: no `Status:` line to check")
        else:
            # The whole paragraph, not just its first line — the Status line
            # wraps, and every sprint it names sits on a later one.
            para, seen = [], False
            for ln in claude_text.splitlines():
                if ln.startswith("Status:"):
                    seen = True
                if seen:
                    if not ln.strip():
                        break
                    para.append(ln)
            # Case-insensitive: "Sprint 013 taught both sides" opening a
            # sentence is correct prose, and a gate that rejects it is asking
            # for a grammatical error to satisfy a string compare.
            if f"sprint {newest}" not in "\n".join(para).lower():
                errors.append(
                    f"CLAUDE.md: the Status paragraph does not mention "
                    f"`sprint {newest}`, the newest sprints/ record — it "
                    "describes a repo that no longer exists (see WI 1928)"
                )

    if errors:
        print("\n".join(errors))
        print(f"check: {len(errors)} problem(s)")
        return 1

    print(
        "check: all JSON parses, all markdown links resolve, "
        "all schemas registered, registry families match both publisher "
        "allowlists, all .ps1 pure ASCII, "
        f"CLAUDE.md current to sprint {newest}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
