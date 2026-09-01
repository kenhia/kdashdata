#!/usr/bin/env python3
"""Repo gate: JSON parses, markdown links resolve, every schema is
registered, every PowerShell script is pure ASCII.

Stdlib only, by design — this repo carries contracts and docs, and its
failure modes are a schema that doesn't parse, a stale cross-reference, a
feed whose schema landed without anyone telling the registry about it, and
(since sprint 004) a non-ASCII byte in the deploy script cleo runs.

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

    if errors:
        print("\n".join(errors))
        print(f"check: {len(errors)} problem(s)")
        return 1

    print(
        "check: all JSON parses, all markdown links resolve, "
        "all schemas registered, all .ps1 pure ASCII"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
