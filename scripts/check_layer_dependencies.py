#!/usr/bin/env python3
"""Check repository include ownership, independently of CMake search paths."""

import argparse
from pathlib import Path
import re
import tempfile


# Header-only runtime and authoring contracts intentionally shared with Nodes.
# Additions require an explicit ownership decision, not a directory-wide exemption.
NODE_CORE_CONTRACTS = set(
    (Path(__file__).resolve().parents[1] / "cmake/node_core_contracts.txt")
    .read_text(encoding="utf-8").splitlines())
NODE_CORE_PATHS = {"include/core/" + name for name in NODE_CORE_CONTRACTS}
SOURCE_SUFFIXES = {".h", ".hpp", ".c", ".cc", ".cpp"}
INCLUDE = re.compile(r'^\s*#\s*include\s*["<]([^">]+)[">]', re.MULTILINE)


def owner(path):
    if path.startswith("include/contracts/") or path in {
        "include/edgeflow/log.h", "include/edgeflow/export.h",
        "include/company_alg_log.h", "include/company_alg_export.h",
        "include/company_alg_version.h"
    }:
        return "Contracts"
    if path.startswith(("include/adapter/", "include/operator/", "include/edgeflow/operator/", "include/platform_mock/", "src/adapter/")) or path in {
        "include/edgeflow/c_api.h", "include/edgeflow/c_api.hpp",
        "include/company_alg_interface.h", "include/company_alg_cpp.hpp"
    }:
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
    if src in {"Contracts", "Integration", "Orchestration", "Capability Nodes", "Model Execution"} and dst == "Support":
        return True
    # Shared contracts must not conceal a dependency on orchestration internals.
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
    if source.startswith("src/adapter/biz/") and dst in {"Model Execution", "Capability Nodes"}:
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
                # Respect quoted local includes, root-relative paths, public
                # include paths and private src paths, including ../ aliases.
                candidates = (source.parent / include, root / include,
                              root / "include" / include, root / "src" / include)
                target = next((p.resolve() for p in candidates if p.is_file()), None)
                if target is None or root not in target.parents:
                    continue  # standard library / fetched dependencies
                target_name = target.relative_to(root).as_posix()
                if forbidden(relative, target_name):
                    errors.append(f"{prefix}: {owner(relative)} -> {owner(target_name)} forbidden ({target_name})")
    return errors


def self_test():
    with tempfile.TemporaryDirectory(prefix="edgeflow-include-contract-") as directory:
        root = Path(directory)
        for name in ("include", "src", "demo"):
            (root / name).mkdir()

        def write(path, content=""):
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(content)
            return target

        for header in ("include/adapter/biz_adapter_interface.h",
                       "include/core/pipeline_validator.h",
                       "include/core/alg_context.h", "include/edgeflow/c_api.h",
                       "include/platform_mock/alg_types.h",
                       "include/platform_mock/operator_data_types.h",
                       "include/platform_mock/operator_types.h",
                       "include/platform_mock/error_codes.h",
                       "src/custom_nodes/domain_node.h"):
            write(header)
        cases = [
            ("src/engine/runtime/bad.cpp", "adapter/biz_adapter_interface.h"),
            ("src/core/bad.cpp", "adapter/biz_adapter_interface.h"),
            ("include/nodes/bad.h", "edgeflow/c_api.h"),
            ("src/common_nodes/bad.cpp", "core/pipeline_validator.h"),
            ("src/custom_nodes/bad.cpp", "core/pipeline_validator.h"),
            ("include/nodes/bad.h", "../adapter/biz_adapter_interface.h"),
            ("src/common_nodes/bad.cpp", "custom_nodes/domain_node.h"),
            ("include/core/session_context.h", "core/pipeline_validator.h"),
        ]
        for header in ("alg_types.h", "operator_data_types.h", "operator_types.h", "error_codes.h"):
            cases.extend((path, f"platform_mock/{header}") for path in (
                "src/core/bad.cpp", "src/common_nodes/bad.cpp", "src/custom_nodes/bad.cpp",
                "include/nodes/bad.h", "src/engine/models/bad.cpp", "src/engine/backends/foreign/bad.cpp"))
            for path in ("src/adapter/biz/good.cpp", "demo/good.cpp"):
                allowed = write(path, f'#include "platform_mock/{header}"\n')
                assert not check(root), (path, header)
                allowed.unlink()
        for vendor, backend in (("onnxruntime_cxx_api.h", "onnxruntime"),
                                ("llama.h", "llama_cpp"), ("whisper.h", "whisper_cpp"),
                                ("kiteLLM.h", "kite_llm")):
            cases.extend((path, vendor) for path in (
                "src/engine/models/bge_embedding/bad.cpp", "include/engine/bad.h",
                "include/nodes/bad.h", "src/engine/backends/foreign/bad.cpp"))
            allowed = write(f"src/engine/backends/{backend}/good.cpp", f'#include "{vendor}"\n')
            assert not check(root), vendor
            allowed.unlink()
        for path, header in cases:
            for directive in (f'#include "{header}"', f'# include <{header}>'):
                file = write(path, directive + "\n")
                assert check(root), (path, directive)
                file.unlink()
        # A local helper must not conceal a reverse dependency.
        write("src/engine/models/demo/model.cpp", '#include "helper.h"\n')
        helper = write("src/engine/models/demo/helper.h", '#include "adapter/biz_adapter_interface.h"\n')
        assert check(root)
        helper.write_text("")
        write("include/nodes/good.h", '#include "core/alg_context.h"\n')
        assert not check(root)
    print("Include ownership positive/negative and indirect dependency tests passed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    errors = check(args.root)
    for error in errors:
        print(error)
    if not errors:
        print("Resolved include ownership and vendor boundaries passed.")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
