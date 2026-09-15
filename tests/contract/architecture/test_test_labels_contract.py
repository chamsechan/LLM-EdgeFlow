#!/usr/bin/env python3
"""Enforce CTest label contracts to prevent CI scope drift.

Validates that:
- Static gates and tooling tests do not run under sanitizer-runtime.
- Core C/C++ runtime suites carry sanitizer-runtime.
- Kite-specific tests carry kite and kite-real labels as appropriate.
- Tests with kite-real also carry kite.
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
    parser.add_argument(
        "--self-test",
        action="store_true",
        help="Run self-tests verifying parser, filter semantics, and error handling",
    )
    return parser.parse_args()


def get_test_inventory(build_dir: Path):
    cmd = ["ctest", "--test-dir", str(build_dir), "--show-only=json-v1"]
    res = subprocess.run(cmd, capture_output=True, text=True)
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
            # Strip trailing comment, e.g. "TypedSuite/0.  # TypeParam = int"
            suite_part = line.split("#", 1)[0].strip()
            # Valid suite names must end with "." and contain no spaces, and not just "."
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
            # Strip trailing comment, e.g. "TestCase  # GetParam() = 1"
            test_part = line.split("#", 1)[0].strip()
            test_words = test_part.split()
            # Valid test cases are single identifiers without whitespace
            if len(test_words) != 1:
                continue
            test_case = test_words[0]
            tests.append(f"{current_suite}{test_case}")
    return tests


def match_gtest_filter(test_name: str, gtest_filter: str) -> bool:
    if not gtest_filter or not gtest_filter.strip():
        # An empty filter (e.g. --gtest_filter=) matches nothing in GoogleTest.
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

    # If positive patterns are omitted but negative patterns exist (e.g. "-Excluded.*"),
    # GoogleTest defaults positive matching to "*".
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
    build_dir: Path, ctest_data: dict, errors: list, runner_fn=None, is_gtest_fn=None
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
            # Explicit GoogleTest target
            if not exe:
                errors.append(
                    f"Failed to enumerate GoogleTest cases from '{test_name}': "
                    f"could not resolve executable from command: {cmd}"
                )
                continue
            if runner_fn is None and not (exe.is_file() and os.access(exe, os.X_OK)):
                errors.append(
                    f"Failed to enumerate GoogleTest cases from '{test_name}': "
                    f"executable '{exe}' not found or not executable."
                )
                continue
            exec_filters.setdefault(exe, []).append(filt)
        elif exe:
            # Check if this executable is a GoogleTest binary
            is_gtest = (
                is_gtest_fn(exe)
                if is_gtest_fn is not None
                else (
                    is_gtest_binary(exe)
                    if (runner_fn is None and exe.is_file())
                    else False
                )
            )
            if is_gtest:
                exec_filters.setdefault(exe, []).append("*")

    covered_test_names = set()
    for exe, filters in exec_filters.items():
        try:
            if runner_fn is not None:
                proc = runner_fn([str(exe), "--gtest_list_tests"])
            else:
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
                    f"Compiled test '{test}' in '{exe.name}' is not covered by any CTest filter."
                )
            else:
                covered_test_names.add(f"{exe.name}:{test}")

    return len(covered_test_names)


def run_self_tests():
    # 1. Issue 1: Typed test parsing with "# TypeParam = ..." and comment stripping
    sample_typed = """
Running main() from /path/to/gtest_main.cc
Note: Random seed = 12345.
NormalSuite.
  NormalTest
TypedSuite/0.  # TypeParam = int
  TypedTestA
  TypedTestB
[Init] Setup completed.
  failed to open some file
TypedSuite/1.  # TypeParam = float
  TypedTestA
  TypedTestB
ValueParamSuite/Inst.
  ValueTest  # GetParam() = 42
"""
    parsed = parse_gtest_list_tests_output(sample_typed)
    expected = [
        "NormalSuite.NormalTest",
        "TypedSuite/0.TypedTestA",
        "TypedSuite/0.TypedTestB",
        "TypedSuite/1.TypedTestA",
        "TypedSuite/1.TypedTestB",
        "ValueParamSuite/Inst.ValueTest",
    ]
    assert parsed == expected, f"Parsed tests mismatch: {parsed} != {expected}"

    # Typed tests must not be falsely covered by a predecessor suite filter
    assert not match_gtest_filter("TypedSuite/0.TypedTestA", "NormalSuite.*")
    assert match_gtest_filter("TypedSuite/0.TypedTestA", "TypedSuite/*")
    assert match_gtest_filter("TypedSuite/0.TypedTestA", "TypedSuite/0.*")
    assert not match_gtest_filter("TypedSuite/0.TypedTestA", "TypedSuite/1.*")

    # 2. Issue 2: Empty filter "--gtest_filter=" does not select any test
    assert not match_gtest_filter("AnySuite.AnyTest", "")
    assert not match_gtest_filter("AnySuite.AnyTest", "   ")
    assert not match_gtest_filter("AnySuite.AnyTest", '""')
    assert not match_gtest_filter("AnySuite.AnyTest", "''")
    assert not is_test_covered("AnySuite.AnyTest", [""])
    assert is_test_covered("AnySuite.AnyTest", ["*"])
    assert is_test_covered("AnySuite.AnyTest", ['"*"'])
    assert is_test_covered("AnySuite.AnyTest", ["", "AnySuite.*"])
    assert not is_test_covered("OtherSuite.AnyTest", ["", "AnySuite.*"])

    # Filter case sensitivity
    assert not match_gtest_filter("AnySuite.AnyTest", "anysuite.*")
    assert match_gtest_filter("AnySuite.AnyTest", "AnySuite.*")

    # Negative pattern handling
    assert match_gtest_filter("Suite.Good", "-Suite.Bad")
    assert not match_gtest_filter("Suite.Bad", "-Suite.Bad")
    assert match_gtest_filter("Suite.Good", "Suite.*:-Suite.Bad")
    assert not match_gtest_filter("Suite.Bad", "Suite.*:-Suite.Bad")
    assert not match_gtest_filter("Suite.Good", "-*")

    # 3. Issue 3: Enumeration failure on explicit GoogleTest target must report errors
    class FakeProc:
        def __init__(self, returncode, stdout="", stderr=""):
            self.returncode = returncode
            self.stdout = stdout
            self.stderr = stderr

    fake_build = Path("/fake/build")
    fake_exe = fake_build / "fake_test_runner"

    # 3a. Target with explicit --gtest_filter fails enumeration (exit code)
    mock_ctest_explicit = {
        "tests": [
            {
                "name": "MockGtest",
                "command": [str(fake_exe), "--gtest_filter=MockSuite.*"],
            }
        ]
    }
    errs = []
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_explicit,
        errs,
        runner_fn=lambda cmd: FakeProc(2, "", "Unknown flag or crash"),
    )
    assert any(
        "Failed to enumerate GoogleTest cases from 'fake_test_runner'" in e
        for e in errs
    ), errs
    assert any("exit code 2" in e for e in errs), errs

    # 3b. Target returns 0 but produces no test cases
    errs = []
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_explicit,
        errs,
        runner_fn=lambda cmd: FakeProc(0, "No tests here\n", ""),
    )
    assert any("no test cases discovered" in e for e in errs), errs

    # 3c. Target execution raises an exception
    errs = []
    def crashing_runner(cmd):
        raise RuntimeError("Subprocess timeout or spawn failure")
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_explicit,
        errs,
        runner_fn=crashing_runner,
    )
    assert any("Failed to execute 'fake_test_runner --gtest_list_tests'" in e for e in errs), errs

    # 3d. Wrapped command resolution with runner_fn (e.g. cmake -E env)
    called_cmds = []
    def recording_runner(cmd):
        called_cmds.append(cmd)
        return FakeProc(0, "MockSuite.\n  Test1\n", "")

    mock_ctest_wrapped = {
        "tests": [
            {
                "name": "MockWrappedGtest",
                "command": [
                    "/usr/bin/cmake",
                    "-E",
                    "env",
                    "EDGEFLOW_VAR=1",
                    str(fake_exe),
                    "--gtest_filter=MockSuite.*",
                ],
            }
        ]
    }
    errs = []
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_wrapped,
        errs,
        runner_fn=recording_runner,
    )
    assert not errs, errs
    assert called_cmds == [[str(fake_exe), "--gtest_list_tests"]], called_cmds

    # 3e. Explicit GoogleTest target with missing or unresolvable executable reports error
    errs = []
    mock_ctest_missing_exe = {
        "tests": [
            {
                "name": "MockMissingGtest",
                "command": [str(fake_build / "nonexistent_bin"), "--gtest_filter=MockSuite.*"],
            }
        ]
    }
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_missing_exe,
        errs,
    )
    assert any("not found or not executable" in e for e in errs), errs

    errs = []
    mock_ctest_no_exe = {
        "tests": [
            {
                "name": "MockNoExeGtest",
                "command": ["--gtest_filter=MockSuite.*"],
            }
        ]
    }
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_no_exe,
        errs,
    )
    assert any("could not resolve executable" in e for e in errs), errs

    # 3f. Explicit --gtest_filter= (empty) must fail coverage check if tests exist
    errs = []
    mock_ctest_empty_filter = {
        "tests": [
            {
                "name": "MockEmptyFilter",
                "command": [str(fake_exe), "--gtest_filter="],
            }
        ]
    }
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_empty_filter,
        errs,
        runner_fn=lambda cmd: FakeProc(0, "Suite.\n  Test1\n", ""),
    )
    assert any("is not covered by any CTest filter" in e for e in errs), errs

    # 3g. Typed test omission reproduction (reproducing missed typed suite)
    mock_ctest_typed = {
        "tests": [
            {
                "name": "MockTypedOnlyNormal",
                "command": [str(fake_exe), "--gtest_filter=NormalSuite.*"],
            }
        ]
    }
    errs = []
    verify_compiled_gtest_coverage(
        fake_build,
        mock_ctest_typed,
        errs,
        runner_fn=lambda cmd: FakeProc(0, sample_typed, ""),
    )
    assert any("TypedSuite/0.TypedTestA" in e for e in errs), errs
    assert any("TypedSuite/0.TypedTestB" in e for e in errs), errs
    assert not any("NormalSuite.NormalTest" in e for e in errs), errs


def main():
    args = parse_args()
    run_self_tests()
    if args.self_test:
        print("✓ All test_test_labels_contract self-tests passed.")
        return

    build_dir = args.build_dir
    if not build_dir.is_dir():
        # Fallback to looking relative to script root if build dir is relative
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

    # Rule 1: static-gate and tooling tests must NOT be in sanitizer-runtime
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

    # Rule 2: kite-real tests must also carry kite label
    for name in kite_real_tests:
        if name not in kite_tests:
            errors.append(f"Kite real test '{name}' must also have 'kite' label.")

    # Rule 3: Key kite targets must be labeled
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

    # Rule 4: Orchestration runtime coverage must survive CI label filtering.
    # Check names explicitly: a non-empty runtime set cannot detect one omission.
    required_sanitizer_runtime = {"PipelineStudioTest"}
    for target in sorted(required_sanitizer_runtime):
        if target not in inventory:
            errors.append(f"Required runtime test '{target}' is missing.")
        elif target not in sanitizer_runtime_tests:
            errors.append(
                f"Expected runtime test '{target}' to be labeled "
                "with 'sanitizer-runtime'."
            )

    # Rule 5: All compiled GoogleTest test cases in binaries executed by CTest
    # must be covered by at least one CTest filter.
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
