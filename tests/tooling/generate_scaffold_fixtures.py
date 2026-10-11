#!/usr/bin/env python3
"""Compile the actual public scaffold's sources and --write-test output.

Only anonymous namespace names are isolated for the existing single-TU runner.
The scaffold owns all generated algorithms and their emitted consumer tests.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scaffold_custom_node.py"


def isolated_namespaces(content, namespace):
    """Keep generated bodies intact while avoiding single-TU local-name clashes."""
    counter = 0

    def replace(match):
        nonlocal counter
        counter += 1
        return f"inline namespace {namespace}_{counter} {{"

    return re.sub(r"namespace\s*\{", replace, content)


def generate(output):
    # Each entry selects a distinct production template or typed call signature.
    requests = [
        ("scaffold_text_identity", ["--in-port", "sentences:TextBatch", "--out-port", "answers:TextBatch"]),
        ("scaffold_audio_identity", ["--in-port", "samples:AudioPcmBatch", "--out-port", "copy:AudioPcmBatch"]),
        ("scaffold_conversion_pending", ["--in-port", "input:TextBatch", "--out-port", "output:Int32Batch"]),
        ("scaffold_llm_call", ["--kind", "model", "--model-capability", "llm"]),
        ("scaffold_embedding_call", ["--kind", "model", "--model-capability", "embedding"]),
        ("scaffold_asr_call", ["--kind", "model", "--model-capability", "asr"]),
        ("scaffold_ocr_call", ["--kind", "model", "--model-capability", "ocr"]),
        ("scaffold_rerank_call", ["--kind", "model", "--model-capability", "rerank"]),
        ("scaffold_prefix_control", ["--control-id", "2000000042"]),
    ]
    sections = ["// Generated from public CLI source and --write-test artifacts.\n"]
    with tempfile.TemporaryDirectory(prefix="scaffold-compile-") as directory:
        root = Path(directory)
        environment = {**os.environ, "LLM_EDGEFLOW_REPO_ROOT": str(root)}
        for name, arguments in requests:
            result = subprocess.run([sys.executable, str(SCRIPT), name, "--write-test", *arguments],
                                    env=environment, capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise RuntimeError(f"Scaffold generation failed for {name}:\n{result.stdout}\n{result.stderr}")
            source = root / "src/custom_nodes" / f"{name}_node.cpp"
            consumer = root / "tests/unit/nodes" / f"test_{name}_node.cpp"
            for kind, path in (("source", source), ("test", consumer)):
                content = path.read_text(encoding="utf-8")
                if kind == "test" and ('node_harness.h' in content or 'node_test_utils.h' in content):
                    raise RuntimeError(f"Generated consumer still depends on retired helpers: {path.name}")
                sections.append(f"\n// Exact generated {kind}: {path.name}\n")
                sections.append(isolated_namespaces(content, f"fixture_{name}_{kind}"))
    output.parent.mkdir(parents=True, exist_ok=True)
    # Publish a complete fixture artifact; interrupted generation retains the prior one.
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=output.parent,
                                     prefix=f".{output.name}.", delete=False) as stream:
        staged = Path(stream.name)
        try:
            stream.write("".join(sections))
            stream.flush()
            os.fsync(stream.fileno())
        except BaseException:
            staged.unlink(missing_ok=True)
            raise
    try:
        os.replace(staged, output)
    finally:
        staged.unlink(missing_ok=True)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: generate_scaffold_fixtures.py OUTPUT.cpp")
    generate(Path(sys.argv[1]).resolve())
