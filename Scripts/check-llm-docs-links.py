#!/usr/bin/env python3
"""Checks that every relative link in the rendered docs (build/llm-docs/**/*.html)
points at a file that exists, and that no page still links to a .md page.

Usage: Scripts/check-llm-docs-links.py [docs-dir]
"""

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r'href="([^"]*)"')


def find_broken_links(docs_dir):
    broken = []
    for page in sorted(docs_dir.rglob("*.html")):
        for target in LINK.findall(page.read_text()):
            if re.match(r"[a-z]+:", target) or target.startswith("#"):
                continue
            path = target.split("#")[0]
            is_markdown_link = path.endswith(".md") and path != page.with_suffix(".md").name
            if is_markdown_link or not (page.parent / path).exists():
                broken.append(f"{page.relative_to(docs_dir)}: {target}")
    return broken


def main():
    docs_dir = Path(sys.argv[1] if len(sys.argv) > 1 else REPO_ROOT / "build" / "llm-docs")
    pages = list(docs_dir.rglob("*.html"))
    broken = find_broken_links(docs_dir)
    print("\n".join(broken))
    print(f"{len(pages)} pages, {len(broken)} broken links")
    sys.exit(1 if broken or not pages else 0)


if __name__ == "__main__":
    main()
