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


def check_delivery_contract(root):
    scripts = root / "delivery" / "scripts"
    scripts.mkdir(parents=True)
    shutil.copy2(ROOT / "scripts/git_branch_upload.sh", scripts)
    binary = root / "delivery" / "bin"
    binary.mkdir()
    mock = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
name, args = Path(sys.argv[0]).name, sys.argv[1:]
log = Path(os.environ["EDGEFLOW_DELIVERY_COMMANDS"])
with log.open("a") as stream:
    stream.write(json.dumps([name, *args]) + "\\n")
mode = os.environ["EDGEFLOW_DELIVERY_MODE"]
if name == "git":
    if args == ["branch", "--show-current"]: print("fix/delivery-test")
    if args == ["diff", "--quiet", "origin/main...HEAD"]: sys.exit(1)
elif name == "gh":
    if args[:2] == ["pr", "view"]:
        fields = args[args.index("--json") + 1]
        if fields == "number": print("94")
        elif fields == "statusCheckRollup": print("6")
        elif fields == "mergeCommit":
            assert args[2] == "94", "query the stable PR number after branch deletion"
            print("" if mode == "missing-sha" else "a" * 40)
    elif args[:2] == ["run", "list"]:
        for flag, value in [("--workflow", "ci.yml"), ("--branch", "main"),
                            ("--event", "push"), ("--commit", "a" * 40)]:
            assert flag in args and args[args.index(flag) + 1] == value, args
        calls = sum(json.loads(line)[:3] == ["gh", "run", "list"] for line in log.read_text().splitlines())
        # No run for this SHA must not select a later main commit or the PR run.
        if mode != "missing-run" and not (mode == "success" and calls == 1): print("123")
    elif args[:2] == ["run", "watch"]:
        assert args == ["run", "watch", "123", "--exit-status"], args
        if mode in ("failure", "cancelled"): sys.exit(1)
    elif args[:2] == ["run", "view"]:
        assert args[2] == "123", args
        if mode != "unconfirmed": print("https://example.invalid/actions/runs/123")
'''
    for path in [*(binary / name for name in ("git", "gh", "sleep")), scripts / "run_all_tests.sh"]:
        path.write_text(mock)
        path.chmod(0o755)
    log = root / "delivery" / "commands.jsonl"
    for mode in ("pr-only", "success", "failure", "cancelled", "missing-run", "missing-sha", "unconfirmed"):
        log.write_text("")
        env = {**os.environ, "PATH": str(binary) + os.pathsep + os.environ["PATH"],
               "EDGEFLOW_DELIVERY_COMMANDS": str(log), "EDGEFLOW_DELIVERY_MODE": mode}
        args = [str(scripts / "git_branch_upload.sh"), "fix(ci): delivery contract", "fix"]
        if mode != "pr-only": args.append("--merge")
        result = run(args, env=env, timeout=15)
        assert (result.returncode == 0) == (mode in ("pr-only", "success")), (mode, result.stdout, result.stderr)
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        assert sum(command[0] == "run_all_tests.sh" for command in commands) == 1
        run_lists = [command for command in commands if command[:3] == ["gh", "run", "list"]]
        watches = [command for command in commands if command[:3] == ["gh", "run", "watch"]]
        if mode == "pr-only":
            assert not run_lists and not watches
            assert not any(command[:3] == ["gh", "pr", "merge"] for command in commands)
        elif mode == "missing-sha":
            assert not run_lists and not watches
            assert "cannot confirm the merge SHA" in result.stdout
        elif mode == "missing-run":
            assert len(run_lists) == 12 and not watches
            assert "is merged, but no main push CI" in result.stdout
        else:
            assert len(watches) == 1
            if mode == "success":
                assert len(run_lists) == 2
                assert "main push CI passed for " + "a" * 40 in result.stdout
            else:
                assert "is merged, but main CI run 123" in result.stdout
                assert "main push CI passed" not in result.stdout


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
                       "ENABLE_WHISPERCPP=OFF", "ENABLE_LLAMACPP=ON", "ENABLE_ONNXRUNTIME=ON",
                       "LLM_EDGEFLOW_TEST_PCH=OFF"):
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
        check_delivery_contract(root)
    print("Canonical gate, failure propagation, CI evidence and merge delivery checks passed.")


if __name__ == "__main__":
    main()
