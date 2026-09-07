#!/usr/bin/env python3
"""Build-only test fixtures: exercise the public CLI and compile its exact output."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
output = Path(sys.argv[1])
output.parent.mkdir(parents=True, exist_ok=True)
cases = [("ScaffoldComputeNode", ["--kind", "compute"]),
         ("ScaffoldConversionNode", ["--out-port", "output:Int32Batch"])]
for kind in ("model", "unary_inference"):
    for cap in ("llm", "embedding", "asr", "ocr", "rerank"):
        if kind == "unary_inference" and cap == "ocr":
            continue  # The CLI rejects this unsupported container contract.
        tag = "Model" if kind == "model" else "Unary"
        cases.append((f"Scaffold{tag}{cap.capitalize()}Node", ["--kind", kind, "-m", cap]))
cases.append(("ScaffoldTutorialLlmNode", ["--kind", "model", "-m", "llm"]))
cases.append(("ScaffoldControlNode", ["--control-id", "2000000042"]))


def apply_documented_text_functions(code):
    """Compile the exact two business function bodies taught in the walkthrough."""
    guide = (root / "doc/dev_guide/first_custom_node.md").read_text(encoding="utf-8")
    for function in ("BuildPrompt", "FormatAnswer"):
        snippet = re.search(r"<!-- starter-example:" + function + r" -->\s*```cpp\n(.*?)\n```",
                            guide, re.DOTALL)
        if not snippet:
            raise RuntimeError(f"Missing documented {function} exercise")
        pattern = (r"(static std::string " + function
                   + r"\(const std::string& text\)\s*\{)\s*return text;\s*\}")
        body = "\n".join("    " + line for line in snippet.group(1).splitlines())
        code, count = re.subn(pattern, lambda match: match.group(1) + "\n" + body + "\n  }", code)
        if count != 1:
            raise RuntimeError(f"Starter no longer has exactly one editable {function}")
    return code


with output.open("w", encoding="utf-8") as stream:
    for name, options in cases:
        code = subprocess.check_output(
            [sys.executable, str(root / "scripts/scaffold_custom_node.py"), name,
             *options, "--description", 'Test fixture: "quoted" \\ 中文\n',
             "--dry-run", "--generate-test"], text=True)
        if name == "ScaffoldTutorialLlmNode":
            code = apply_documented_text_functions(code)
        stream.write(code)
