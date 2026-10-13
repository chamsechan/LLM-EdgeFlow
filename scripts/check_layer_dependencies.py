#!/usr/bin/env python3
"""Check repository include ownership, independently of CMake search paths."""

import argparse
from pathlib import Path
import re


# 有意与 Node 共享的纯头文件运行时及编写契约。
# 新增条目需要明确的归属决定，而不是按目录整体豁免。
NODE_CORE_CONTRACTS = set(
    (Path(__file__).resolve().parents[1] / "cmake_ext/node_core_contracts.txt")
    .read_text(encoding="utf-8").splitlines())
NODE_CORE_PATHS = {"include/core/" + name for name in NODE_CORE_CONTRACTS}
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)


def owner(path):
    if path.startswith("include/contracts/") or path in {
        "include/edgeflow/log.h", "include/edgeflow/export.h"
    }:
        return "Contracts"
    if path.startswith(("include/adapter/", "include/edgeflow/operator/", "include/platform_mock/", "src/adapter/")):
        return "Integration"
    if path.startswith(("include/core/", "src/core/")):
        return "Orchestration"
    if path.startswith(("include/nodes/", "src/common_nodes/", "src/custom_nodes/")):
        return "Capability Nodes"
    if path.startswith(("include/engine/", "src/engine/")):
        return "Model Execution"
    if path.startswith("demo/"):
        return "Demo"
    return "Support"


def forbidden(source, target):
    src, dst = owner(source), owner(target)
    if source.startswith(("src/adapter/input/", "src/adapter/output/")) and target in {
        "include/adapter/operator/mock/platform_value_binding.h",
        "include/platform_mock/operator_data_types.h",
        "include/platform_mock/operator_types.h",
        "include/edgeflow/operator/types.h",
        "include/edgeflow/operator/interface.h",
    }:
        return True
    if src in {"Contracts", "Integration", "Orchestration", "Capability Nodes", "Model Execution"} and dst == "Support":
        return True
    # 共享契约不得隐藏对编排内部实现的依赖。
    if source in NODE_CORE_PATHS and dst == "Orchestration" and target not in NODE_CORE_PATHS:
        return True
    if src == "Contracts" and dst not in {"Contracts", "Support"}:
        return True
    if src == "Model Execution" and dst in {"Integration", "Orchestration", "Capability Nodes"}:
        return True
    if src == "Orchestration" and dst == "Integration":
        return True
    if src == "Capability Nodes":
        if dst == "Integration":
            return True
        if dst == "Orchestration" and target not in NODE_CORE_PATHS:
            return True
    if src in {"Orchestration", "Capability Nodes"} and target.startswith(
        ("src/engine/models/", "src/engine/backends/")
    ):
        return True
    if src == "Orchestration" and dst == "Capability Nodes":
        return True
    if target.startswith("src/custom_nodes/") and (
        src in {"Orchestration", "Model Execution"}
        or source.startswith(("src/common_nodes/", "include/nodes/"))
    ):
        return True
    if source.startswith(("src/adapter/input/", "src/adapter/output/")) and dst in {"Model Execution", "Capability Nodes"}:
        return True
    if src == "Demo" and target.startswith(("src/", "include/adapter/", "include/core/", "include/nodes/", "include/engine/", "include/contracts/")):
        return True
    return False


def vendor_owners(include):
    name = Path(include).name
    if name.startswith("onnxruntime") and name.endswith((".h", ".hpp")):
        return {"onnxruntime"}
    if name == "llama.h":
        return {"llama_cpp"}
    if name == "whisper.h":
        return {"whisper_cpp"}
    if name == "kiteLLM.h":
        return {"kite_llm"}
    if name.startswith("ggml") and name.endswith(".h"):
        return {"llama_cpp", "whisper_cpp"}
    return None


def check(root):
    root = root.resolve()
    errors = []
    for required in ("include", "src", "demo"):
        if not (root / required).is_dir():
            errors.append(f"Missing source directory: {root / required}")
    if errors:
        return errors
    for directory in ("include", "src", "demo"):
        for source in sorted((root / directory).rglob("*")):
            if not source.is_file() or source.suffix not in SOURCE_SUFFIXES:
                continue
            relative = source.relative_to(root).as_posix()
            text = source.read_text(encoding="utf-8").replace("\\\n", "")
            text = re.sub(r'/\*.*?\*/', lambda m: "\n" * m[0].count("\n"), text, flags=re.DOTALL)
            for match in INCLUDE.finditer(text):
                include = match[1]
                line = text.count("\n", 0, match.start()) + 1
                prefix = f"{relative}:{line}: {include}"
                vendors = vendor_owners(include)
                if vendors and not any(relative.startswith(f"src/engine/backends/{vendor}/") for vendor in vendors):
                    errors.append(f"{prefix}: vendor header outside its concrete Backend")
                    continue
                # 支持带引号的本地 include、根相对路径、公共 include 路径
                # 和私有 src 路径，包括 ../ 别名。
                candidates = (source.parent / include, root / include,
                              root / "include" / include, root / "src" / include)
                target = next((p.resolve() for p in candidates if p.is_file()), None)
                if target is None or root not in target.parents:
                    continue  # 标准库 / 拉取的依赖
                target_name = target.relative_to(root).as_posix()
                if forbidden(relative, target_name):
                    errors.append(f"{prefix}: {owner(relative)} -> {owner(target_name)} forbidden ({target_name})")
    return errors



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    errors = check(args.root)
    for error in errors:
        print(error)
    if not errors:
        print("Resolved include ownership and vendor boundaries passed.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
