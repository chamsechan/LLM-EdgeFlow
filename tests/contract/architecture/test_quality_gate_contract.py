#!/usr/bin/env python3
"""Exercise canonical gate arguments, failure propagation and CI evidence."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]


def run(args, **kwargs):
    return subprocess.run(args, text=True, capture_output=True, **kwargs)


def main():
    real_cmake = shutil.which("cmake")
    real_ctest = shutil.which("ctest")
    with tempfile.TemporaryDirectory(prefix="edgeflow-quality-contract-") as directory:
        root = Path(directory)
        scripts = root / "scripts"
        scripts.mkdir()
        for name in ("run_all_tests.sh", "detect_cmake_generator.sh", "generate_acceptance_evidence.sh"):
            shutil.copy2(ROOT / "scripts" / name, scripts / name)
        (scripts / "format.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
        (scripts / "format.sh").chmod(0o755)
        assert run(["git", "init", "-q", str(root)]).returncode == 0
        assert run(["git", "-C", str(root), "add", "scripts"]).returncode == 0
        assert run(["git", "-C", str(root), "-c", "user.name=Gate test", "-c",
                    "user.email=gate@example.invalid", "commit", "-qm", "fixture"]).returncode == 0
        assert run(["git", "-C", str(root), "branch", "-M", "main"]).returncode == 0
        binary = root / "bin"
        binary.mkdir()
        mock = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
with open(os.environ["EDGEFLOW_GATE_COMMANDS"], "a") as stream:
    stream.write(json.dumps([Path(sys.argv[0]).name, *sys.argv[1:]]) + "\\n")
if Path(sys.argv[0]).name == "ctest":
    mode = os.environ.get("EDGEFLOW_CTEST_MODE", "success")
    if mode == "failure": sys.exit(8)
    if mode == "empty": sys.exit(1 if "--no-tests=error" in sys.argv else 0)
'''
        for name in ("cmake", "ctest"):
            path = binary / name
            path.write_text(mock)
            path.chmod(0o755)
        log = root / "commands.jsonl"
        env = {**os.environ, "PATH": str(binary) + os.pathsep + os.environ["PATH"],
               "EDGEFLOW_GATE_COMMANDS": str(log), "LLM_EDGEFLOW_JOBS": "1",
               "LLM_EDGEFLOW_LINKER": "auto"}
        result = run([str(scripts / "run_all_tests.sh")], env=env)
        assert result.returncode == 0, result.stdout + result.stderr
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        configure = next(command for command in commands if command[0] == "cmake" and "-S" in command)
        for option in ("BUILD_TESTING=ON", "ENABLE_SANITIZERS=OFF", "ENABLE_KITELLM=OFF",
                       "ENABLE_WHISPERCPP=OFF", "ENABLE_LLAMACPP=ON", "ENABLE_ONNXRUNTIME=ON"):
            assert "-D" + option in configure, (option, configure)
        assert any("--no-tests=error" in command for command in commands if command[0] == "ctest")
        for mode in ("failure", "empty"):
            result = run([str(scripts / "run_all_tests.sh")], env={**env, "EDGEFLOW_CTEST_MODE": mode})
            assert result.returncode != 0, (mode, result.stdout)
            assert "All required development gates passed" not in result.stdout

        # Apply the gate's actual configure options to a real CMake cache that
        # previously disabled tests; this is not merely a text search for ON.
        fixture = root / "fixture"
        fixture.mkdir()
        (fixture / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.16)\nproject(GateFixture NONE)\n'
            'include(CTest)\nif(BUILD_TESTING)\n'
            'add_test(NAME required COMMAND "${CMAKE_COMMAND}" -E true)\nendif()\n')
        build = root / "fixture-build"
        assert run([real_cmake, "-S", str(fixture), "-B", str(build), "-DBUILD_TESTING=OFF"]).returncode == 0
        options = [arg for arg in configure if arg.startswith("-D")]
        assert run([real_cmake, "-S", str(fixture), "-B", str(build), *options]).returncode == 0
        inventory = run([real_ctest, "--test-dir", str(build), "--show-only=json-v1"])
        assert [test["name"] for test in json.loads(inventory.stdout)["tests"]] == ["required"]

        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        assert "WHISPER_GATE_RESULT: ${{ needs.whisper-asr.result }}" in workflow
        assert "KITELLM_GATE_RESULT: ${{ needs.kite-llm.result }}" in workflow
        evidence = root / "evidence.json"
        for state in ("success", "failure", "skipped", "cancelled"):
            evidence_env = {**os.environ, "WHISPER_GATE_RESULT": state,
                            "KITELLM_GATE_RESULT": "skipped"}
            result = run([str(scripts / "generate_acceptance_evidence.sh"), str(evidence),
                          "success", "failure", "cancelled"], env=evidence_env)
            assert result.returncode == 0, result.stdout + result.stderr
            gates = json.loads(evidence.read_text())["gates"]
            assert gates == {"canonical_run_all_tests": "success",
                             "full_address_undefined_sanitizer": "failure",
                             "real_c_abi_and_public_profile": "cancelled",
                             "whisper_asr_backend_and_real_profile": state,
                             "kitellm_private_release_and_real_gguf": "skipped"}
    print("Canonical gate, cached test option, failure propagation and CI evidence passed.")


if __name__ == "__main__":
    main()
