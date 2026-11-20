#!/usr/bin/env python3
"""Verify internal Markdown file links and heading anchors across repository docs."""

from pathlib import Path
import re
import subprocess
import sys
import unicodedata
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
FENCE_RE = re.compile(r"^\s*(`{3,}|~{3,})")
HEADING_RE = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*#*\s*$")
INLINE_CODE_RE = re.compile(r"`[^`]*`")
LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)]+)\)")
EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "file://", "conversation://")


def github_slug(heading: str) -> str:
    """Compute a GitHub-flavored Markdown heading anchor preserving CJK characters."""
    text = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", heading)
    text = text.replace("`", "").strip().lower()
    out: list[str] = []
    for ch in text:
        if ch in (" ", "\t", "-"):
            out.append("-")
            continue
        cat = unicodedata.category(ch)
        if cat[0] in ("L", "N") or ch == "_":
            out.append(ch)
    return "".join(out)


def extract_anchors(path: Path) -> set[str]:
    anchors: set[str] = set()
    counts: dict[str, int] = {}
    in_fence: str | None = None
    for line in path.read_text(encoding="utf-8").splitlines():
        fence = FENCE_RE.match(line)
        if fence:
            marker = fence.group(1)[0]
            if in_fence is None:
                in_fence = marker
            elif in_fence == marker:
                in_fence = None
            continue
        if in_fence is not None:
            continue
        match = HEADING_RE.match(line)
        if not match:
            continue
        base = github_slug(match.group(1))
        if not base:
            continue
        seen = counts.get(base, 0)
        counts[base] = seen + 1
        anchors.add(base if seen == 0 else f"{base}-{seen}")
    return anchors


def markdown_files() -> list[Path]:
    tracked = subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=ROOT,
    ).decode().split("\0")
    selected = []
    for rel in sorted(set(tracked)):
        if not rel or not rel.endswith(".md"):
            continue
        if rel in ("README.md", "AGENTS.md") or rel.startswith("docs/"):
            path = ROOT / rel
            if path.is_file():
                selected.append(path)
    return selected


def main() -> int:
    files = markdown_files()
    anchor_cache: dict[Path, set[str]] = {}
    errors: list[str] = []
    link_count = 0

    for source in files:
        rel_source = source.relative_to(ROOT).as_posix()
        in_fence: str | None = None
        for lineno, raw_line in enumerate(source.read_text(encoding="utf-8").splitlines(), 1):
            fence = FENCE_RE.match(raw_line)
            if fence:
                marker = fence.group(1)[0]
                if in_fence is None:
                    in_fence = marker
                elif in_fence == marker:
                    in_fence = None
                continue
            if in_fence is not None:
                continue
            line = INLINE_CODE_RE.sub("", raw_line)
            for match in LINK_RE.finditer(line):
                raw_target = match.group(1).strip()
                if not raw_target:
                    continue
                if raw_target.startswith("<") and raw_target.endswith(">"):
                    raw_target = raw_target[1:-1].strip()
                else:
                    raw_target = raw_target.split()[0]
                if raw_target.startswith(EXTERNAL_PREFIXES):
                    continue
                link_count += 1
                path_part, _, frag = raw_target.partition("#")
                path_part = unquote(path_part)
                frag = unquote(frag)
                target = source if not path_part else (source.parent / path_part).resolve()
                try:
                    target.relative_to(ROOT)
                except ValueError:
                    errors.append(f"{rel_source}:{lineno}: {raw_target} (points outside repository)")
                    continue
                if not target.exists():
                    errors.append(f"{rel_source}:{lineno}: {raw_target} (target not found)")
                    continue
                if frag and target.is_file() and target.suffix.lower() == ".md":
                    anchors = anchor_cache.get(target)
                    if anchors is None:
                        anchors = extract_anchors(target)
                        anchor_cache[target] = anchors
                    if frag.lower() not in anchors:
                        errors.append(
                            f"{rel_source}:{lineno}: {raw_target} (missing heading anchor #{frag})"
                        )

    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(f"Found {len(errors)} broken link(s) across {len(files)} Markdown files.", file=sys.stderr)
        return 1
    print(f"Checked {link_count} local links across {len(files)} Markdown files: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
