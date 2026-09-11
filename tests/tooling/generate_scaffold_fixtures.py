#!/usr/bin/env python3
"""Build-only test fixtures: exercise the public CLI and compile its exact output."""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

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

    # Compile and execute the exact standalone tests produced for developers.
    # Legacy snippet coverage above must not conceal a broken --write-test path.
    standalone_cases = [
        ("ScaffoldWrittenTextNode", ["--kind", "compute"]),
        ("ScaffoldWrittenAudioNode", ["--kind", "compute", "--in-port", "input:AudioPcmBatch",
                                      "--out-port", "output:AudioPcmBatch"]),
        ("ScaffoldWrittenControlNode", ["--control-id", "2000000043"]),
    ]
    for kind in ("model", "unary_inference"):
        for capability in ("llm", "embedding", "asr", "ocr", "rerank"):
            if kind == "unary_inference" and capability == "ocr":
                continue
            tag = "Model" if kind == "model" else "Unary"
            standalone_cases.append((f"ScaffoldWritten{tag}{capability.capitalize()}Node",
                                     ["--kind", kind, "--model-capability", capability]))
    with tempfile.TemporaryDirectory(prefix="edgeflow-written-fixtures-") as directory:
        fixture_root = Path(directory)
        for relative in ("src/custom_nodes/CMakeLists.txt", "cmake_ext/CustomNodeTests.cmake"):
            target = fixture_root / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text((root / relative).read_text(encoding="utf-8"), encoding="utf-8")
        env = dict(os.environ, LLM_EDGEFLOW_REPO_ROOT=str(fixture_root))
        for name, options in standalone_cases:
            subprocess.run([sys.executable, str(root / "scripts/scaffold_custom_node.py"),
                            name, *options, "--write-test", "--add-to-cmake"],
                           env=env, text=True, capture_output=True, check=True)
        for source in sorted((fixture_root / "src/custom_nodes").glob("*.cpp")):
            stream.write(source.read_text(encoding="utf-8"))
        for test in sorted((fixture_root / "tests/unit/nodes").glob("test_*.cpp")):
            stream.write(test.read_text(encoding="utf-8"))
