#!/usr/bin/env python3
"""通过公开 CLI 验证仓库的 Markdown 检查器。"""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[3] / "scripts/check_doc_links.py"


class MarkdownLinksContractTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="edgeflow-doc-links-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name) / "repo"
        self.root.mkdir()

    def write(self, relative, text):
        destination = self.root / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(text, encoding="utf-8")

    def run_checker(self):
        result = subprocess.run(
            [sys.executable, str(CHECKER), "--root", str(self.root)],
            capture_output=True, text=True, encoding="utf-8", timeout=20,
        )
        self.assertEqual(result.stderr, "", result.stderr)
        return result

    def add_valid_cross_file_anchor(self):
        self.write("guide.md", "# Stable\n")
        self.write("README.md", "[guide](guide.md#stable)\n")

    def test_checks_unicode_duplicate_manual_and_quoted_file_destinations(self):
        self.write("guide.md", "# 准备 运行\n# Same\n# Same\n"
                   '<a id="manual-target"></a>\n')
        self.write("docs/My Page.md", "# Space Heading\n")
        self.write("docs/route(v2).md", "# Route\n")
        self.write("README.md", """[unicode](guide.md#准备-运行)
[first](guide.md#same)
[duplicate](guide.md#same-1)
[manual](guide.md#manual-target)
[space](<docs/My Page.md#space-heading>)
[balanced](docs/route(v2).md#route)
<a href="guide.md#manual-target">manual HTML link</a>
`[inline example](missing-inline.md)`
```markdown
[fenced example](missing-fence.md#missing)
```
[remote](https://example.invalid/missing.md#missing)
[mail](mailto:nobody@example.invalid)
[protocol relative](//example.invalid/missing.md)
""")
        result = self.run_checker()
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(
            result.stdout,
            "Markdown links passed: 4 Markdown files, 7 relative links, "
            "7 cross-file heading anchors.\n",
        )

    def test_missing_target_and_heading_report_the_source_path(self):
        self.add_valid_cross_file_anchor()
        cases = (
            ("absent.md", "missing link target: absent.md"),
            ("guide.md#absent", "missing heading anchor: guide.md#absent"),
        )
        for target, diagnostic in cases:
            with self.subTest(target=target):
                self.write("README.md", f"[guide](guide.md#stable)\n[bad]({target})\n")
                result = self.run_checker()
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(f"README.md:2: {diagnostic}\n", result.stdout)
                self.assertIn("Markdown links FAILED:", result.stdout)

    def test_existing_destination_outside_root_is_rejected(self):
        self.add_valid_cross_file_anchor()
        outside = self.root.parent / "outside.md"
        outside.write_text("# Outside\n", encoding="utf-8")
        self.write("README.md", "[guide](guide.md#stable)\n"
                   "[outside](../outside.md#outside)\n")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn(
            "README.md:2: link leaves the repository: ../outside.md#outside\n",
            result.stdout,
        )

    def test_empty_repository_cannot_pass(self):
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no Markdown files were found; refusing to pass an empty check\n",
                      result.stdout)
        self.assertIn("Markdown links FAILED: 0 Markdown files, 0 relative links, "
                      "0 cross-file heading anchors.", result.stdout)

    def test_same_file_anchor_alone_cannot_satisfy_cross_file_check(self):
        self.write("README.md", "# Local\n[local](#local)\n")
        result = self.run_checker()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("no cross-file heading anchors were checked; link extraction is broken\n",
                      result.stdout)
        self.assertNotIn("missing heading anchor", result.stdout)
        self.assertIn("Markdown links FAILED: 1 Markdown files, 1 relative links, "
                      "0 cross-file heading anchors.", result.stdout)


if __name__ == "__main__":
    unittest.main()
