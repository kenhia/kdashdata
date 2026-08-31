#!/usr/bin/env python3
"""Repo gate: every JSON file parses; every relative markdown link resolves.

Stdlib only, by design — this repo carries contracts and docs, and its
failure modes are a schema that doesn't parse and a stale cross-reference.
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SKIP_DIRS = {".git", ".scratch"}
LINK_RE = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")


def repo_files(suffix: str):
    for path in sorted(ROOT.rglob(f"*{suffix}")):
        rel = path.relative_to(ROOT)
        if not any(part in SKIP_DIRS for part in rel.parts):
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

    if errors:
        print("\n".join(errors))
        print(f"check: {len(errors)} problem(s)")
        return 1

    print("check: all JSON parses, all markdown links resolve")
    return 0


if __name__ == "__main__":
    sys.exit(main())
