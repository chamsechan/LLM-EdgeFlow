#!/usr/bin/env python3
"""Compile the actual public scaffold's sources and --write-test output.

Only anonymous namespace names are isolated for the existing single-TU runner.
The scaffold owns all generated algorithms and their emitted consumer tests.
The first custom Node tutorial is replayed on the same scaffold output: each
anchored code block replaces its same-named definition, and every stage is
compiled, executed and checked against the Catalog facts the tutorial states.
"""

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "tools/scaffold_custom_node.py"
TUTORIAL = ROOT / "doc/dev_guide/first_custom_node.md"
TUTORIAL_NODE = "my_text_normalize"
ANCHOR = re.compile(r"<!-- tutorial-code: (stage\d+) (node|test) -->\n```cpp\n(.*?)\n```", re.S)
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'')
# Catalog facts each tutorial stage states: own config fields and model slots.
TUTORIAL_CATALOG = {
    "stage1": ([], []),
    "stage2": (["replacement"], []),
    "stage3": (["replacement", "max_tokens", "bind_model"], [("model", "llm", "bind_model")]),
}


def isolated_namespaces(content, namespace):
    """Keep generated bodies intact while avoiding single-TU local-name clashes."""
    counter = 0

    def replace(match):
        nonlocal counter
        counter += 1
        return f"inline namespace {namespace}_{counter} {{"

    return re.sub(r"namespace\s*\{", replace, content)


def tutorial_blocks(text):
    blocks = {}
    for stage, kind, code in ANCHOR.findall(text):
        blocks.setdefault(stage, {"node": [], "test": []})[kind].append(code)
    if sorted(blocks) != sorted(TUTORIAL_CATALOG):
        raise RuntimeError(f"Tutorial stages {sorted(blocks)} differ from {sorted(TUTORIAL_CATALOG)}")
    for stage, kinds in blocks.items():
        if not kinds["node"] or len(kinds["test"]) != 1:
            raise RuntimeError(f"Tutorial {stage} needs node blocks and exactly one test block")
    return blocks


def definitions(lines):
    """Top-level definitions starting at column 0 as (name, kind, first, last) line ranges."""
    found = []
    index = 0
    while index < len(lines):
        line = lines[index]
        if not line or line[0] in " \t}#/" or line.startswith(("namespace", "REGISTER_FUNCTION_NODE")):
            index += 1
            continue
        match = re.match(r"struct\s+(\w+)", line)
        name, kind = (match.group(1), "struct") if match else (re.search(r"(\w+)\s*\(", line).group(1), "function")
        depth, opened, last = 0, False, index
        while True:
            code = LITERAL.sub("", lines[last])
            depth += code.count("{") - code.count("}")
            opened |= "{" in code
            if (opened and depth == 0) or (not opened and code.rstrip().endswith(";")):
                break
            last += 1
        found.append((name, kind, index, last))
        index = last + 1
    return found


def apply_node_block(source, block):
    lines = source.split("\n")
    block_lines = block.split("\n")
    for name, kind, first, last in definitions(block_lines):
        replacement = block_lines[first:last + 1]
        current = definitions(lines)
        existing = [item for item in current if item[0] == name]
        if existing:
            _, _, start, end = existing[0]
            lines[start:end + 1] = replacement
        elif kind == "struct":
            end = [item for item in current if item[1] == "struct"][-1][3]
            lines[end + 1:end + 1] = [""] + replacement
        else:
            raise RuntimeError(f"Tutorial block defines unknown function {name}")
    return "\n".join(lines)


def apply_test_block(test, block):
    lines = test.split("\n")
    includes = [line for line in block.split("\n") if line.startswith("#include")]
    body = [line for line in block.split("\n") if not line.startswith("#include")]
    while body and not body[0]:
        body.pop(0)
    anchor = lines.index('#include "tests/support/node_probe.h"')
    lines[anchor + 1:anchor + 1] = [line for line in includes if line not in lines]
    first = next(i for i, line in enumerate(lines) if line.startswith("TEST("))
    last = next(i for i, line in enumerate(lines) if line.startswith("}  // namespace"))
    lines[first:last] = body
    return "\n".join(lines)


def catalog_check(stage, node_type):
    fields, models = TUTORIAL_CATALOG[stage]
    field_checks = "".join(
        f'''  EXPECT_TRUE(std::any_of(definition->config_fields.begin(), definition->config_fields.end(),
                          [](const auto& field) {{ return field.name == "{name}"; }})) << "{name}";
''' for name in fields)
    model_checks = "".join(
        f'''  EXPECT_EQ(definition->model_dependencies[{i}].name, "{name}");
  EXPECT_EQ(definition->model_dependencies[{i}].model_type, "{model_type}");
  EXPECT_EQ(definition->model_dependencies[{i}].config_field, "{field}");
''' for i, (name, model_type, field) in enumerate(models))
    exact = "" if models else f"  EXPECT_EQ(definition->config_fields.size(), {len(fields)}u);\n"
    return f'''
// Catalog facts stated by the tutorial for {stage}.
namespace llm_edgeflow {{
TEST(FirstCustomNodeTutorialTest, {stage}_CatalogMatchesTutorial) {{
  const auto definition = NodeRegistry::Instance().Find("{node_type}");
  ASSERT_TRUE(definition.has_value());
  EXPECT_FALSE(definition->description.empty());
{exact}{field_checks}  ASSERT_EQ(definition->model_dependencies.size(), {len(models)}u);
{model_checks}}}
}}  // namespace llm_edgeflow
'''


def tutorial_sections(root, environment, tutorial):
    blocks = tutorial_blocks(Path(tutorial).read_text(encoding="utf-8"))
    result = subprocess.run([sys.executable, str(SCRIPT), TUTORIAL_NODE, "--write-test"],
                            env=environment, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"Scaffold generation failed for the tutorial:\n{result.stdout}\n{result.stderr}")
    source = (root / "src/custom_nodes" / f"{TUTORIAL_NODE}_node.cpp").read_text(encoding="utf-8")
    test = (root / "tests/unit/nodes" / f"test_{TUTORIAL_NODE}_node.cpp").read_text(encoding="utf-8")
    sections = []
    for stage in sorted(blocks):
        for block in blocks[stage]["node"]:
            source = apply_node_block(source, block)
        test = apply_test_block(test, blocks[stage]["test"][0])
        node_type = f"tutorial_{stage}_{TUTORIAL_NODE}"
        for kind, content in (("source", source), ("test", test)):
            sections.append(f"\n// Tutorial {stage} {kind}: {TUTORIAL.name} applied to scaffold output\n")
            sections.append(isolated_namespaces(content.replace(TUTORIAL_NODE, node_type),
                                                f"fixture_tutorial_{stage}_{kind}"))
        sections.append(catalog_check(stage, node_type))
    return sections


def generate(output, tutorial=TUTORIAL):
    # Each entry selects a distinct body of the single Node structure or typed call signature.
    requests = [
        ("scaffold_text_identity", ["--in-port", "sentences:TextBatch", "--out-port", "answers:TextBatch"]),
        ("scaffold_audio_identity", ["--in-port", "samples:AudioPcmBatch", "--out-port", "copy:AudioPcmBatch"]),
        ("scaffold_rule_match_identity", ["--in-port", "matches:RuleMatchBatch", "--out-port", "copy:RuleMatchBatch"]),
        ("scaffold_conversion_pending", ["--in-port", "input:TextBatch", "--out-port", "output:Int32Batch"]),
        ("scaffold_llm_call", ["--model-capability", "llm"]),
        ("scaffold_embedding_call", ["--model-capability", "embedding"]),
        ("scaffold_asr_call", ["--model-capability", "asr"]),
        ("scaffold_ocr_call", ["--model-capability", "ocr"]),
        ("scaffold_rerank_call", ["--model-capability", "rerank"]),
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
        sections.extend(tutorial_sections(root, environment, tutorial))
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
