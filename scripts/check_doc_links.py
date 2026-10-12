#!/usr/bin/env python3
"""Check relative links and heading anchors in the repository's Markdown files."""

import argparse
from pathlib import Path
import re
import string
import subprocess
import unicodedata
from urllib.parse import unquote


ATX_HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
INLINE_LINK_START = re.compile(r"\]\(")
INLINE_LINK_END = re.compile(
    r'''(?:[ \t]+(?:"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|\((?:\\.|[^()\\])*\)))?[ \t]*\)''')
HTML_TARGET = re.compile(r"\b(?:href|src)=\"([^\"]+)\"")
HTML_ANCHOR = re.compile(r"<a\s+(?:id|name)=\"([^\"]+)\"")
INLINE_CODE = re.compile(r"`[^`]*`")
URL_SCHEME = re.compile(r"^[A-Za-z][A-Za-z0-9+.-]*:")
FENCE = re.compile(r"^\s*(```|~~~)")
WALK_EXCLUDES = {".git", "3rdparty", "results", "output", "Testing"}


def link_destination(text, position):
    """读取尖括号包裹或括号平衡的裸目标地址及其结束偏移。"""
    angle_delimited = text[position:position + 1] == "<"
    if angle_delimited:
        position += 1
    destination, depth = [], 0
    while position < len(text):
        char = text[position]
        if char == "\\" and position + 1 < len(text) and text[position + 1] in string.punctuation:
            destination.append(text[position + 1])
            position += 2
            continue
        if angle_delimited:
            if char == ">":
                return "".join(destination), position + 1
            if char in "<\r\n":
                return None
        elif char <= " " or char == "\x7f":
            break
        elif char == "(":
            depth += 1
        elif char == ")":
            if depth == 0:
                break
            depth -= 1
        destination.append(char)
        position += 1
    if angle_delimited or depth:
        return None
    return "".join(destination), position


def inline_link_targets(text):
    """逐个产出单行内联链接目标，不含可选的链接标题。"""
    position = 0
    while match := INLINE_LINK_START.search(text, position):
        position = match.end()
        start = position
        while start < len(text) and text[start] in " \t":
            start += 1
        parsed = link_destination(text, start)
        if parsed is None:
            continue
        destination, end = parsed
        suffix = INLINE_LINK_END.match(text, end)
        if suffix:
            position = suffix.end()
            yield destination


def slugify(heading):
    """GitHub 标题锚点：转小写，去掉标点/符号，空格转为 '-'。"""
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
    """产出代码围栏之外的 (行号, 文本)。"""
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
    """返回 Markdown 文件中所有相对链接的 (errors, counters)。"""
    root = root.resolve()
    errors, cache = [], {}
    counters = {"files": 0, "links": 0, "anchor_links": 0}
    for path in markdown_files(root):
        counters["files"] += 1
        relative = path.relative_to(root)
        for number, line in prose_lines(path):
            prose = INLINE_CODE.sub("", line)
            for target in [*inline_link_targets(prose), *HTML_TARGET.findall(prose)]:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors, counters = check(args.root)
    for error in errors:
        print(error)
    summary = (f"{counters['files']} Markdown files, {counters['links']} relative links, "
               f"{counters['anchor_links']} cross-file heading anchors")
    print(f"Markdown links {'FAILED' if errors else 'passed'}: {summary}.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
