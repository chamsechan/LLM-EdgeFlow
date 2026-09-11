#!/usr/bin/env python3
"""Enforce CTest label contracts to prevent CI scope drift.

Validates that:
- Static gates and tooling tests do not run under sanitizer-runtime.
- Core C/C++ runtime suites carry sanitizer-runtime.
- Kite-specific tests carry kite and kite-real labels as appropriate.
- Tests with kite-real also carry kite.
"""

import argparse
import json
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
    return inventory


def main():
    args = parse_args()
    build_dir = args.build_dir
    if not build_dir.is_dir():
        # Fallback to looking relative to script root if build dir is relative
        candidate = Path(__file__).resolve().parents[3] / build_dir
        if candidate.is_dir():
            build_dir = candidate
        else:
            sys.stderr.write(f"Build directory not found: {args.build_dir}\n")
            sys.exit(1)

    inventory = get_test_inventory(build_dir)
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

    if errors:
        sys.stderr.write("CTest label contract violations:\n")
        for err in errors:
            sys.stderr.write(f"  - {err}\n")
        sys.exit(1)

    print(
        f"✓ CTest label contracts verified ({len(inventory)} tests, "
        f"{len(sanitizer_runtime_tests)} sanitizer-runtime, "
        f"{len(kite_tests)} kite, {len(kite_real_tests)} kite-real)."
    )


if __name__ == "__main__":
    main()
