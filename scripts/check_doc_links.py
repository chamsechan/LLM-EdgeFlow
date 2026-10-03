#!/usr/bin/env python3
"""Check relative links and heading anchors in the repository's Markdown files."""

import argparse
from pathlib import Path
import re
import subprocess
import tempfile
import unicodedata
from urllib.parse import unquote


ATX_HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
INLINE_LINK = re.compile(r"\]\(\s*<?([^)\s>]+)>?(?:\s+\"[^\"]*\")?\s*\)")
HTML_TARGET = re.compile(r"\b(?:href|src)=\"([^\"]+)\"")
HTML_ANCHOR = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"")
INLINE_CODE = re.compile(r"`[^`]*`")
URL_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
FENCE = re.compile(r"^\s*(```|~~~)")
WALK_EXCLUDES = {".git", "3rdparty", "results", "output", "Testing"}


def slugify(heading):
    """GitHub heading anchor: lowercase, drop punctuation/symbols, spaces to '-'."""
    text = re.sub(r"`([^`]*)`", r"\1", heading)
    text = re.sub(r"!?\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"<[^>]+>", "", text)
    slug = []
    for char in text.strip().lower():
        if char == " ":
            slug.append("-")
        elif char in "-_":
            slug.append(char)
        elif unicodedata.category(char)[0] not in "PS":
            slug.append(char)
    return "".join(slug)


def prose_lines(path):
    """Yield (line number, text) outside fenced code blocks."""
    fence = None
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        match = FENCE.match(line)
        if match:
            if fence is None:
                fence = match.group(1)
            elif match.group(1) == fence:
                fence = None
            continue
        if fence is None:
            yield number, line


def anchors(path, cache):
    if path not in cache:
        found, seen = set(), {}
        for _, line in prose_lines(path):
            found.update(HTML_ANCHOR.findall(line))
            heading = ATX_HEADING.match(line)
            if heading:
                slug = slugify(heading.group(2))
                count = seen.get(slug, 0)
                seen[slug] = count + 1
                found.add(slug if count == 0 else f"{slug}-{count}")
        cache[path] = found
    return cache[path]


def markdown_files(root):
    try:
        listed = subprocess.run(
            ["git", "-C", str(root), "ls-files", "-z", "--cached", "--others",
             "--exclude-standard", "--", "*.md"],
            check=True, capture_output=True).stdout.decode("utf-8").split("\0")
        files = [root / name for name in listed if name]
    except (OSError, subprocess.CalledProcessError):
        files = [path for path in root.rglob("*.md")
                 if not any(part in WALK_EXCLUDES or part.startswith("build")
                            for part in path.relative_to(root).parts[:-1])]
    return sorted(path for path in files if path.is_file())


def check(root):
    """Return (errors, counters) for every relative link in the Markdown files."""
    root = root.resolve()
    errors, cache = [], {}
    counters = {"files": 0, "links": 0, "anchor_links": 0}
    for path in markdown_files(root):
        counters["files"] += 1
        relative = path.relative_to(root)
        for number, line in prose_lines(path):
            prose = INLINE_CODE.sub("", line)
            for target in INLINE_LINK.findall(prose) + HTML_TARGET.findall(prose):
                if URL_SCHEME.match(target) or target.startswith("//"):
                    continue
                counters["links"] += 1
                file_part, _, anchor = target.partition("#")
                file_part, anchor = unquote(file_part), unquote(anchor)
                if not file_part:
                    destination = path
                elif file_part.startswith("/"):
                    destination = root / file_part.lstrip("/")
                else:
                    destination = path.parent / file_part
                destination = destination.resolve()
                if destination != root and root not in destination.parents:
                    errors.append(f"{relative}:{number}: link leaves the repository: {target}")
                    continue
                if not destination.exists():
                    errors.append(f"{relative}:{number}: missing link target: {target}")
                    continue
                if not anchor or destination.suffix != ".md" or not destination.is_file():
                    continue
                if destination != path:
                    counters["anchor_links"] += 1
                if anchor not in anchors(destination, cache):
                    errors.append(f"{relative}:{number}: missing heading anchor: {target}")
    if counters["files"] == 0:
        errors.append("no Markdown files were found; refusing to pass an empty check")
    elif counters["anchor_links"] == 0:
        errors.append("no cross-file heading anchors were checked; link extraction is broken")
    return errors, counters


def self_test():
    def write(root, name, text):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    with tempfile.TemporaryDirectory(prefix="doc-links-") as directory:
        root = Path(directory)
        write(root, "guide/target.md", "# Title\n## 6. 输出容量与生命周期\n## Repeat\n## Repeat\n"
                                       "## `plans/` 目录\n<a id=\"manual\"></a>\n")
        write(root, "guide/code.cpp", "int main() {}\n")
        write(root, "index.md",
              "[a](guide/target.md#6-输出容量与生命周期) [b](guide/target.md#repeat-1)\n"
              "[c](guide/target.md#plans-目录) [d](guide/) [e](#local) [f](guide/code.cpp#L1)\n"
              "[g](guide/target.md#manual) [h](https://example.com/x.md#none)\n"
              "<img src=\"guide/code.cpp\"> `[skip](missing.md)`\n"
              "```markdown\n[skip](missing.md)\n```\n## Local\n")
        errors, counters = check(root)
        assert errors == [], errors
        # Seven inline links and one img src; the https link is skipped.
        assert counters == {"files": 2, "links": 8, "anchor_links": 4}, counters

        write(root, "broken.md", "[x](guide/missing.md) [y](guide/target.md#nope) [z](../outside.md)\n")
        errors, _ = check(root)
        assert len(errors) == 3, errors
        assert "missing link target" in errors[0], errors
        assert "missing heading anchor" in errors[1], errors
        assert "leaves the repository" in errors[2], errors

    with tempfile.TemporaryDirectory(prefix="doc-links-empty-") as directory:
        errors, _ = check(Path(directory))
        assert any("no Markdown files" in error for error in errors), errors

    with tempfile.TemporaryDirectory(prefix="doc-links-unlinked-") as directory:
        write(Path(directory), "only.md", "# Only\n[self](#only)\n")
        errors, _ = check(Path(directory))
        assert any("no cross-file heading anchors" in error for error in errors), errors
    print("Markdown link checker self-tests passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    errors, counters = check(args.root)
    for error in errors:
        print(error)
    summary = (f"{counters['files']} Markdown files, {counters['links']} relative links, "
               f"{counters['anchor_links']} cross-file heading anchors")
    print(f"Markdown links {'FAILED' if errors else 'passed'}: {summary}.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
