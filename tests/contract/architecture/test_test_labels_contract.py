#!/usr/bin/env python3
"""强制 CTest 标签契约，防止 CI 覆盖范围漂移。

校验：
- 静态门禁和工具测试不在 sanitizer-runtime 下运行。
- 核心 C/C++ 运行时测试套件带有 sanitizer-runtime 标签。
- Kite 专属测试按需带有 kite 和 kite-real 标签。
- 带 kite-real 的测试也带有 kite。
"""

import argparse
import fnmatch
import json
import os
from pathlib import Path
import subprocess
import sys


def parse_args():
    parser = argparse.ArgumentParser(description="Verify CTest label contracts")
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="Path to build directory containing CTest configuration",
    )
    return parser.parse_args()


def get_test_inventory(build_dir: Path):
    cmd = ["ctest", "--show-only=json-v1"]
    res = subprocess.run(cmd, cwd=build_dir, capture_output=True, text=True)
    if res.returncode != 0:
        sys.stderr.write(f"Failed to query ctest inventory: {res.stderr}\n")
        sys.exit(res.returncode)
    data = json.loads(res.stdout)
    inventory = {}
    for test in data.get("tests", []):
        name = test["name"]
        props = {p["name"]: p["value"] for p in test.get("properties", [])}
        labels = set(props.get("LABELS", []))
        inventory[name] = labels
    return inventory, data


def is_gtest_binary(exe: Path) -> bool:
    try:
        with open(exe, "rb") as f:
            target = b"gtest_list_tests"
            chunk_size = 65536
            overlap = len(target)
            prev = b""
            while True:
                chunk = f.read(chunk_size)
                if not chunk:
                    return False
                if target in (prev + chunk):
                    return True
                prev = chunk[-overlap:]
    except Exception:
        return False


def parse_gtest_list_tests_output(output: str) -> list:
    current_suite = ""
    tests = []
    for line in output.splitlines():
        if not line or line.isspace():
            continue
        if not line[0].isspace():
            # 去掉行尾注释，如 "TypedSuite/0.  # TypeParam = int"
            suite_part = line.split("#", 1)[0].strip()
            # 合法的套件名必须以 "." 结尾、不含空格，且不能只是 "."
            if (
                len(suite_part) <= 1
                or not suite_part.endswith(".")
                or any(c.isspace() for c in suite_part)
            ):
                continue
            current_suite = suite_part
        else:
            if not current_suite:
                continue
            # 去掉行尾注释，如 "TestCase  # GetParam() = 1"
            test_part = line.split("#", 1)[0].strip()
            test_words = test_part.split()
            # 合法的测试用例是不含空白的单个标识符
            if len(test_words) != 1:
                continue
            test_case = test_words[0]
            tests.append(f"{current_suite}{test_case}")
    return tests


def match_gtest_filter(test_name: str, gtest_filter: str) -> bool:
    if not gtest_filter or not gtest_filter.strip():
        # 空过滤器 (如 --gtest_filter=) 在 GoogleTest 中不匹配任何测试。
        return False
    clean_filter = gtest_filter.strip()
    if (clean_filter.startswith('"') and clean_filter.endswith('"')) or (
        clean_filter.startswith("'") and clean_filter.endswith("'")
    ):
        clean_filter = clean_filter[1:-1].strip()
        if not clean_filter:
            return False
    parts = clean_filter.split("-", 1)
    pos_part = parts[0]
    neg_part = parts[1] if len(parts) > 1 else ""
    pos_pats = [p for p in pos_part.split(":") if p]
    neg_pats = [p for p in neg_part.split(":") if p]

    # 省略正向模式但存在反向模式 (如 "-Excluded.*") 时，
    # GoogleTest 默认正向匹配 "*"。
    if pos_pats:
        pos_match = any(fnmatch.fnmatchcase(test_name, pat) for pat in pos_pats)
    else:
        pos_match = len(parts) > 1

    neg_match = (
        any(fnmatch.fnmatchcase(test_name, pat) for pat in neg_pats)
        if neg_pats
        else False
    )
    return pos_match and not neg_match


def is_test_covered(test_name: str, filters: list) -> bool:
    return any(match_gtest_filter(test_name, f) for f in filters)


def resolve_command_executable(cmd: list, build_dir: Path) -> Path | None:
    wrappers = {
        "env",
        "cmake",
        "ctest",
        "python",
        "python3",
        "python3.11",
        "bash",
        "sh",
        "valgrind",
    }
    resolved_build_dir = build_dir.resolve()
    for arg in cmd:
        if not arg or arg.startswith("-"):
            continue
        if "=" in arg and not arg.startswith("/") and not arg.startswith("./"):
            continue
        p = Path(arg)
        if p.name in wrappers:
            continue
        if p.suffix in (".sh", ".py", ".cmake", ".mjs", ".js"):
            continue
        if ".so" in p.name:
            continue
        resolved_p = (
            p.resolve()
            if p.is_absolute()
            else (resolved_build_dir / p).resolve()
        )
        try:
            if resolved_p == resolved_build_dir or resolved_p.is_relative_to(
                resolved_build_dir
            ):
                return resolved_p
        except AttributeError:
            if str(resolved_p).startswith(str(resolved_build_dir)):
                return resolved_p
    return None


def verify_compiled_gtest_coverage(
    build_dir: Path, ctest_data: dict, errors: list
) -> int:
    exec_filters = {}
    resolved_build_dir = build_dir.resolve()
    for test in ctest_data.get("tests", []):
        test_name = test.get("name", "unknown")
        cmd = test.get("command", [])
        filt = None
        for i, arg in enumerate(cmd):
            if arg.startswith("--gtest_filter="):
                filt = arg[len("--gtest_filter=") :]
            elif arg == "--gtest_filter" and i + 1 < len(cmd):
                filt = cmd[i + 1]

        exe = resolve_command_executable(cmd, resolved_build_dir)

        if filt is not None:
            # 显式的 GoogleTest 目标
            if not exe:
                errors.append(
                    f"Failed to enumerate GoogleTest cases from '{test_name}': "
                    f"could not resolve executable from command: {cmd}"
                )
                continue
            if not (exe.is_file() and os.access(exe, os.X_OK)):
                errors.append(
                    f"Failed to enumerate GoogleTest cases from '{test_name}': "
                    f"executable '{exe}' not found or not executable."
                )
                continue
            exec_filters.setdefault(exe, []).append(filt)
        elif exe:
            # 检查该可执行文件是否为 GoogleTest 二进制
            if exe.is_file() and is_gtest_binary(exe):
                exec_filters.setdefault(exe, []).append("*")

    covered_test_names = set()
    for exe, filters in exec_filters.items():
        try:
            proc = subprocess.run(
                [str(exe), "--gtest_list_tests"],
                capture_output=True,
                text=True,
                timeout=30,
            )
        except Exception as ex:
            errors.append(
                f"Failed to execute '{exe.name} --gtest_list_tests': {ex}"
            )
            continue

        if proc.returncode != 0:
            err_msg = proc.stderr.strip() or proc.stdout.strip()
            errors.append(
                f"Failed to enumerate GoogleTest cases from '{exe.name}' "
                f"(exit code {proc.returncode}): {err_msg}"
            )
            continue

        tests = parse_gtest_list_tests_output(proc.stdout)
        if not tests:
            errors.append(
                f"Failed to enumerate GoogleTest cases from '{exe.name}': "
                "no test cases discovered."
            )
            continue

        for test in tests:
            if "DISABLED_" in test:
                continue
            if not is_test_covered(test, filters):
                errors.append(
                    f"Compiled test '{test}' in '{exe.name}' is not covered by any CTest filter. "
                    "Add its suite to a filter in tests/RuntimeTests.cmake "
                    "(edgeflow_add_runner_test), or move the case into an existing suite "
                    "such as CustomNodeCatalogTest."
                )
            else:
                covered_test_names.add(f"{exe.name}:{test}")

    return len(covered_test_names)


def main():
    args = parse_args()

    build_dir = args.build_dir
    if not build_dir.is_dir():
        # 构建目录为相对路径时，回退为相对脚本根目录查找
        candidate = Path(__file__).resolve().parents[3] / build_dir
        if candidate.is_dir():
            build_dir = candidate
        else:
            sys.stderr.write(f"Build directory not found: {args.build_dir}\n")
            sys.exit(1)

    inventory, ctest_data = get_test_inventory(build_dir)
    if not inventory:
        sys.stderr.write("No tests found in CTest inventory\n")
        sys.exit(1)

    errors = []

    sanitizer_runtime_tests = {
        name for name, labels in inventory.items() if "sanitizer-runtime" in labels
    }
    kite_tests = {name for name, labels in inventory.items() if "kite" in labels}
    kite_real_tests = {
        name for name, labels in inventory.items() if "kite-real" in labels
    }
    static_tests = {
        name for name, labels in inventory.items() if "static-gate" in labels
    }
    tooling_tests = {
        name for name, labels in inventory.items() if "tooling" in labels
    }

    if not sanitizer_runtime_tests:
        errors.append("Expected non-empty sanitizer-runtime test set.")
    if not kite_tests:
        errors.append("Expected non-empty kite test set.")
    if not kite_real_tests:
        errors.append("Expected non-empty kite-real test set.")
    if not static_tests:
        errors.append("Expected non-empty static-gate test set.")
    if not tooling_tests:
        errors.append("Expected non-empty tooling test set.")

    # 规则 1：静态门禁和工具测试不得属于 sanitizer-runtime
    for name in static_tests:
        if name in sanitizer_runtime_tests:
            errors.append(
                f"Static test '{name}' must not have 'sanitizer-runtime' label."
            )
    for name in tooling_tests:
        if name in sanitizer_runtime_tests:
            errors.append(
                f"Tooling test '{name}' must not have 'sanitizer-runtime' label."
            )

    # 规则 2：kite-real 测试必须同时带有 kite 标签
    for name in kite_real_tests:
        if name not in kite_tests:
            errors.append(f"Kite real test '{name}' must also have 'kite' label.")

    # 规则 3：关键的 kite 目标必须带标签
    required_kite = {
        "CatalogContractSsotTest",
        "ModelBackendDecouplingTest",
        "ModelBackendPipelineTest",
        "LlamaCppBackendTest",
        "DemoRunnerTest",
    }
    for target in required_kite:
        if target in inventory and target not in kite_tests:
            errors.append(f"Expected test '{target}' to be labeled with 'kite'.")

    required_kite_real = {
        "LlamaCppBackendTest",
        "DemoRunnerTest",
    }
    for target in required_kite_real:
        if target in inventory and target not in kite_real_tests:
            errors.append(f"Expected test '{target}' to be labeled with 'kite-real'.")

    # 规则 4：编排层运行时覆盖必须经得起 CI 标签过滤。
    # 显式检查名称：仅检查运行时集合非空无法发现单个遗漏。
    required_sanitizer_runtime = {"PipelineStudioTest"}
    for target in sorted(required_sanitizer_runtime):
        if target not in inventory:
            errors.append(f"Required runtime test '{target}' is missing.")
        elif target not in sanitizer_runtime_tests:
            errors.append(
                f"Expected runtime test '{target}' to be labeled "
                "with 'sanitizer-runtime'."
            )

    # 规则 5：CTest 执行的二进制中所有已编译的 GoogleTest 用例
    # 都必须至少被一个 CTest 过滤器覆盖。
    covered_gtest_count = verify_compiled_gtest_coverage(
        build_dir, ctest_data, errors
    )

    if errors:
        sys.stderr.write("CTest contract violations:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        sys.exit(1)

    print(
        f"✓ CTest label and coverage contracts verified ({len(inventory)} tests, "
        f"{covered_gtest_count} compiled GoogleTest cases, "
        f"{len(sanitizer_runtime_tests)} sanitizer-runtime, "
        f"{len(kite_tests)} kite, {len(kite_real_tests)} kite-real)."
    )


if __name__ == "__main__":
    main()
