#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"

python3 - "${PROJECT_ROOT}/scripts/configure_build.sh" <<'PY'
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

helper = Path(sys.argv[1])
with tempfile.TemporaryDirectory(prefix="edgeflow-configure-contract-") as directory:
    root = Path(directory)
    binary = root / "bin"
    binary.mkdir()
    # An isolated PATH makes both Ninja availability branches deterministic.
    for name in ("bash", "sh", "env", "dirname", "sed", "grep", "awk", "head", "cut"):
        command = shutil.which(name)
        if command:
            (binary / name).symlink_to(command)
    cmake = binary / "cmake"
    cmake.write_text("#!" + sys.executable + '\n'
                     'import json, os, sys\n'
                     'print(json.dumps(sys.argv[1:]))\n'
                     'sys.exit(int(os.environ.get("MOCK_CMAKE_EXIT", "0")))\n')
    cmake.chmod(0o755)
    env = {**os.environ, "PATH": str(binary)}
    source = root / "source with spaces"
    source.mkdir()
    options = ["-DMODEL_ROOT=/path with spaces/weights", "-DVALUES=one;two three"]

    def check(name, generator, cached=None):
        build = root / name
        build.mkdir(exist_ok=True)
        if cached:
            (build / "CMakeCache.txt").write_text("CMAKE_GENERATOR:INTERNAL=" + cached + "\n")
        result = subprocess.run([str(helper), str(source), str(build), "minimal", *options],
                                env=env, text=True, capture_output=True)
        assert result.returncode == 0, (result.stdout, result.stderr)
        args = json.loads(result.stdout)
        assert "--preset=minimal" in args, args
        for option, expected in (("-S", str(source)), ("-B", str(build)), ("-G", generator)):
            assert args[args.index(option) + 1] == expected, args
        assert args[-len(options):] == options, args

    check("fresh without ninja", "Unix Makefiles")
    (binary / "ninja").symlink_to(shutil.which("true"))
    check("fresh with ninja", "Ninja")
    check("existing make build", "Unix Makefiles", "Unix Makefiles")
    check("existing ninja build", "Ninja", "Ninja")
    check("existing multi config", "Ninja Multi-Config", "Ninja Multi-Config")
    (binary / "ninja").unlink()
    check("cached ninja without binary", "Ninja", "Ninja")
    for _ in range(3):
        check("existing make build", "Unix Makefiles", "Unix Makefiles")
    failure = subprocess.run([str(helper), str(source), str(root / "failure"), "minimal"],
                             env={**env, "MOCK_CMAKE_EXIT": "23"}, capture_output=True)
    assert failure.returncode == 23, failure
    for missing in ([], [str(source)], [str(source), str(root / "missing")]):
        result = subprocess.run([str(helper), *missing], env=env, capture_output=True)
        assert result.returncode != 0, missing
print("Native preset configure generator selection, argument boundaries and failure propagation passed.")
PY
