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
    if args == ["rev-parse", "HEAD"]: print("b" * 40)
    if args == ["diff", "--quiet", "origin/main...HEAD"]: sys.exit(1)
    if args[:3] == ["merge-base", "--is-ancestor", "origin/main"] and mode == "main-advanced":
        calls = sum(json.loads(line)[:3] == ["git", "fetch", "origin"] for line in log.read_text().splitlines())
        if calls > 1: sys.exit(1)
elif name == "gh":
    if args[:2] == ["pr", "view"]:
        fields = args[args.index("--json") + 1]
        if fields == "number": print("94")
        elif fields == "statusCheckRollup": print("6")
        elif fields == "headRefOid":
            assert args[2] == "94", "query the stable PR number"
            if mode == "head-query-failure": sys.exit(1)
            print("" if mode == "missing-head" else "invalid" if mode == "invalid-head"
                  else ("c" if mode == "head-changed" else "b") * 40)
        elif fields == "mergeCommit":
            assert args[2] == "94", "query the stable PR number after branch deletion"
            print("" if mode == "missing-sha" else "a" * 40)
    elif args[:2] == ["pr", "merge"]:
        assert "--match-head-commit" in args, args
        assert args[args.index("--match-head-commit") + 1] == "b" * 40, args
        if mode == "head-race":
            print("PR head changed before merge", file=sys.stderr)
            sys.exit(1)
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
    before_merge_failures = ("main-advanced", "head-changed", "missing-head", "invalid-head", "head-query-failure")
    for mode in ("pr-only", "success", "failure", "cancelled", "missing-run", "missing-sha",
                 "unconfirmed", *before_merge_failures, "head-race"):
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
        merges = [command for command in commands if command[:3] == ["gh", "pr", "merge"]]
        for merge in merges:
            assert merge == ["gh", "pr", "merge", "94", "--merge", "--delete-branch",
                             "--match-head-commit", "b" * 40], merge
            assert ["git", "merge-base", "--is-ancestor", "origin/main", "b" * 40] in commands
            assert ["git", "rev-list", "--merges", "origin/main.." + "b" * 40] in commands
        if mode == "pr-only" or mode in before_merge_failures:
            assert not run_lists and not watches
            assert not merges
            if mode == "main-advanced":
                assert "origin/main is not an ancestor" in result.stdout
            elif mode == "head-changed":
                assert "head changed since the verified branch was pushed" in result.stdout
            elif mode in ("missing-head", "invalid-head", "head-query-failure"):
                assert "cannot confirm the head SHA" in result.stdout
        elif mode == "head-race":
            assert len(merges) == 1 and not run_lists and not watches
            assert "PR head changed before merge" in result.stderr
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


def check_delivery_history_contract(root):
    """Use real Git graphs and a local remote to prove the PR history policy."""
    fixture = root / "delivery history"
    project = fixture / "project"
    project.mkdir(parents=True)
    remote = fixture / "origin.git"
    binary = fixture / "bin"
    binary.mkdir()
    log = fixture / "commands.jsonl"
    env = {**os.environ, "PATH": str(binary) + os.pathsep + os.environ["PATH"],
           "EDGEFLOW_DELIVERY_COMMANDS": str(log)}

    def git(*args):
        result = run(["git", "-c", "commit.gpgsign=false", *args], cwd=project, env=env)
        assert result.returncode == 0, (args, result.stdout, result.stderr)
        return result.stdout.strip()

    def commit_file(name, content):
        (project / name).write_text(content)
        git("add", name)
        git("commit", "-qm", name)

    mock = '''#!/usr/bin/env python3
import json, os, subprocess, sys
from pathlib import Path
name, args = Path(sys.argv[0]).name, sys.argv[1:]
with open(os.environ["EDGEFLOW_DELIVERY_COMMANDS"], "a") as stream:
    stream.write(json.dumps([name, *args]) + "\\n")
mode = os.environ.get("EDGEFLOW_HISTORY_HEAD_MODE")
def remote_head():
    return subprocess.check_output(["git", "ls-remote", "origin",
                                    "refs/heads/" + os.environ["EDGEFLOW_HISTORY_BRANCH"]],
                                   text=True).split()[0]
def advance_head():
    subprocess.run(["git", "push", "origin", os.environ["EDGEFLOW_HISTORY_UPDATED_HEAD"]
                    + ":refs/heads/" + os.environ["EDGEFLOW_HISTORY_BRANCH"]],
                   check=True, capture_output=True, text=True)
if name == "gh":
    if args[:2] == ["pr", "view"]:
        fields = args[args.index("--json") + 1]
        if fields == "number": print("94")
        elif fields == "statusCheckRollup": print("1")
        elif fields == "headRefOid": print(remote_head())
    elif args[:2] == ["pr", "checks"] and mode in ("linear-update", "merge-update"):
        advance_head()
    elif args[:2] == ["pr", "merge"]:
        if mode == "merge-race": advance_head()
        expected = args[args.index("--match-head-commit") + 1] if "--match-head-commit" in args else None
        if expected is not None and expected != remote_head():
            print("PR head does not match checked commit", file=sys.stderr)
            sys.exit(1)
        print("MOCK_MERGE_ACCEPTED")
'''
    scripts = project / "scripts"
    scripts.mkdir()
    shutil.copy2(ROOT / "scripts/git_branch_upload.sh", scripts)
    for path in (binary / "gh", scripts / "run_all_tests.sh"):
        path.write_text(mock)
        path.chmod(0o755)
    git("init", "-q", "-b", "main")
    git("config", "user.name", "History contract")
    git("config", "user.email", "history@example.invalid")
    git("add", "scripts")
    git("commit", "-qm", "initial")
    git("switch", "-c", "fix/prior")
    commit_file("prior.txt", "already merged stage\n")
    git("switch", "main")
    git("merge", "--no-ff", "-m", "prior stage", "fix/prior")
    git("init", "--bare", "-q", str(remote))
    git("remote", "add", "origin", str(remote))
    git("push", "-u", "origin", "main")
    assert git("rev-list", "--merges", "origin/main"), "fixture main must contain a legitimate merge"

    def deliver(accepted, error=None):
        log.write_text("")
        branch = git("branch", "--show-current")
        original_main = git("ls-remote", "origin", "refs/heads/main")
        result = run([str(scripts / "git_branch_upload.sh"), "fix(ci): history contract", "fix"],
                     cwd=project, env=env, timeout=15)
        assert (result.returncode == 0) == accepted, (branch, result.stdout, result.stderr)
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        assert sum(command[0] == "run_all_tests.sh" for command in commands) == int(accepted)
        assert not any(command[:3] == ["gh", "pr", "merge"] for command in commands)
        remote_branch = git("ls-remote", "origin", "refs/heads/" + branch)
        if accepted:
            assert remote_branch.split()[0] == git("rev-parse", "HEAD"), remote_branch
        else:
            assert not commands and not remote_branch, (commands, remote_branch)
            assert error in result.stdout, result.stdout
        assert git("ls-remote", "origin", "refs/heads/main") == original_main

    git("switch", "-c", "fix/clean", "origin/main")
    commit_file("clean.txt", "first change\n")
    commit_file("clean.txt", "first change\nsecond change\n")
    deliver(True)

    git("switch", "-c", "fix/outdated", "origin/main")
    commit_file("outdated.txt", "current stage\n")
    git("switch", "main")
    commit_file("main-update.txt", "previous stage completed\n")
    git("push", "origin", "main")
    git("switch", "fix/outdated")
    deliver(False, "origin/main is not an ancestor")

    git("switch", "-c", "fix/reverse-merged")
    git("merge", "--no-ff", "-m", "merge main into working branch", "origin/main")
    deliver(False, "contains merge commits above origin/main")

    git("switch", "-c", "fix/nested", "origin/main")
    commit_file("nested.txt", "current stage\n")
    git("switch", "-c", "fix/nested-side", "origin/main")
    commit_file("nested-side.txt", "another stage\n")
    git("switch", "fix/nested")
    git("merge", "--no-ff", "-m", "merge another working branch", "fix/nested-side")
    deliver(False, "contains merge commits above origin/main")

    git("switch", "fix/outdated")
    git("rebase", "origin/main")
    deliver(True)

    for mode in ("linear-update", "merge-update", "merge-race"):
        git("switch", "-c", "fix/" + mode, "origin/main")
        commit_file(mode + ".txt", "verified change\n")
        checked_head = git("rev-parse", "HEAD")
        side = git("commit-tree", "HEAD^{tree}", "-p", checked_head, "-m", "concurrent update")
        updated_head = side if mode == "linear-update" else git(
            "commit-tree", "HEAD^{tree}", "-p", checked_head, "-p", side, "-m", "forbidden merge")
        if mode != "linear-update":
            assert git("rev-list", "--merges", "origin/main.." + updated_head)
        log.write_text("")
        original_main = git("ls-remote", "origin", "refs/heads/main")
        branch = git("branch", "--show-current")
        race_env = {**env, "EDGEFLOW_HISTORY_HEAD_MODE": mode,
                    "EDGEFLOW_HISTORY_BRANCH": branch, "EDGEFLOW_HISTORY_UPDATED_HEAD": updated_head}
        result = run([str(scripts / "git_branch_upload.sh"), "fix(ci): PR head contract", "fix", "--merge"],
                     cwd=project, env=race_env, timeout=15)
        assert result.returncode != 0, (mode, result.stdout, result.stderr)
        commands = [json.loads(line) for line in log.read_text().splitlines()]
        assert sum(command[0] == "run_all_tests.sh" for command in commands) == 1
        merges = [command for command in commands if command[:3] == ["gh", "pr", "merge"]]
        assert len(merges) == int(mode == "merge-race"), (mode, commands)
        if mode == "merge-race":
            assert merges[0][-2:] == ["--match-head-commit", checked_head], merges
            assert "PR head does not match checked commit" in result.stderr
        else:
            assert "head changed since the verified branch was pushed" in result.stdout
        assert "MOCK_MERGE_ACCEPTED" not in result.stdout
        assert not any(command[:3] == ["gh", "run", "list"] for command in commands)
        assert git("rev-parse", "HEAD") == checked_head
        assert git("ls-remote", "origin", "refs/heads/" + branch).split()[0] == updated_head
        assert git("ls-remote", "origin", "refs/heads/main") == original_main

MOCK_COMMAND = '''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
name, args = Path(sys.argv[0]).name, sys.argv[1:]
with open(os.environ["EDGEFLOW_GATE_COMMANDS"], "a") as stream:
    stream.write(json.dumps({"command": [name, *args],
                             "executable": str(Path(sys.argv[0]).resolve()),
                             "cwd": str(Path.cwd()),
                             "arch_wrapped": os.environ.get("EDGEFLOW_ARCH_WRAPPED"),
                             "ccache_dir": os.environ.get("CCACHE_DIR")}) + "\\n")
stage = "configure" if name == "cmake" and "--build" not in args else "build" if name == "cmake" else name
if os.environ.get("EDGEFLOW_FAIL_STAGE") == stage:
    sys.exit(9)
if name == "ctest" and os.environ.get("EDGEFLOW_EMPTY_TESTS"):
    sys.exit(1 if "--no-tests=error" in args else 0)
if name == "setarch":
    assert args[1] == "-R", args
    os.execvpe(args[2], args[2:], {**os.environ, "EDGEFLOW_ARCH_WRAPPED": "1"})
if stage == "build":
    build = Path(args[args.index("--build") + 1])
    build.mkdir(parents=True, exist_ok=True)
    for binary in ("test_real_models_e2e", "alg_demo"):
        path = build / binary
        path.write_text(Path(sys.argv[0]).read_text())
        path.chmod(0o755)
'''


def mock_project(root):
    scripts = root / "scripts"
    scripts.mkdir(parents=True)
    for name in ("run_all_tests.sh", "configure_build.sh", "run_sanitizers.sh",
                 "run_real_model_e2e.sh", "generate_acceptance_evidence.sh"):
        shutil.copy2(ROOT / "scripts" / name, scripts / name)
    shutil.copy2(ROOT / "CMakePresets.json", root / "CMakePresets.json")
    (scripts / "format.sh").write_text("#!/usr/bin/env bash\nexit 0\n")
    (scripts / "format.sh").chmod(0o755)
    assert run(["git", "init", "-q", str(root)]).returncode == 0
    binary = root / "bin"
    binary.mkdir()
    for path in (binary / name for name in ("cmake", "ctest", "ccache", "setarch", "fetch_real_test_models.sh")):
        path.write_text(MOCK_COMMAND)
        path.chmod(0o755)
    fetch = scripts / "fetch_real_test_models.sh"
    fetch.write_text('#!/usr/bin/env bash\nexec "$(dirname "${BASH_SOURCE[0]}")/../bin/fetch_real_test_models.sh" "$@"\n')
    fetch.chmod(0o755)
    assert run(["git", "-C", str(root), "add", "scripts", "CMakePresets.json"]).returncode == 0
    assert run(["git", "-C", str(root), "-c", "user.name=Gate test", "-c",
                "user.email=gate@example.invalid", "commit", "-qm", "fixture"]).returncode == 0
    assert run(["git", "-C", str(root), "branch", "-M", "main"]).returncode == 0
    log = root / "commands.jsonl"
    env = {**os.environ, "PATH": str(binary) + os.pathsep + os.environ["PATH"],
           "EDGEFLOW_GATE_COMMANDS": str(log), "LLM_EDGEFLOW_JOBS": "1",
           "LLM_EDGEFLOW_LINKER": "auto", "LLM_EDGEFLOW_SANITIZERS": "undefined",
           "CCACHE_DIR": str(root / "compiler cache")}
    env.pop("LLM_EDGEFLOW_SANITIZER_BUILD_DIR", None)
    env.pop("LLM_EDGEFLOW_REAL_MODEL_BUILD_DIR", None)
    return env, log


def invoke(root, env, log, script, *args, failure=None, empty=False):
    log.write_text("")
    result = run([str(root / "scripts" / script), *args], cwd=root,
                 env={**env, "EDGEFLOW_FAIL_STAGE": failure or "",
                      "EDGEFLOW_EMPTY_TESTS": "1" if empty else ""}, timeout=30)
    records = [json.loads(line) for line in log.read_text().splitlines()]
    assert (result.returncode != 0) == bool(failure or empty), (script, args, result.stdout, result.stderr)
    return result, records


def configure_command(records, preset, build):
    commands = [row["command"] for row in records]
    configure = [command for command in commands if command[0] == "cmake" and "-S" in command]
    assert len(configure) == 1, commands
    command = configure[0]
    assert "--preset=" + preset in command, command
    assert command[command.index("-B") + 1] == str(build), command
    builds = [command for command in commands if command[:2] == ["cmake", "--build"]]
    assert len(builds) == 1 and builds[0][2] == str(build), builds
    return command


def check_ctest_records(records, build):
    tests = [row for row in records if row["command"][0] == "ctest"]
    assert len(tests) == 1, tests
    test = tests[0]
    assert test["cwd"] == str(build), test
    assert "--test-dir" not in test["command"], test
    assert "--no-tests=error" in test["command"], test
    return test["command"]


def check_gate_contract(root, env, log):
    _, records = invoke(root, env, log, "run_all_tests.sh")
    configure = configure_command(records, "dev-gate", root / "build")
    check_ctest_records(records, root / "build")
    for failure in ("configure", "build", "ctest"):
        result, records = invoke(root, env, log, "run_all_tests.sh", failure=failure)
        assert "All required development gates passed" not in result.stdout
        if failure in ("configure", "build"):
            assert all(row["command"][0] != "ctest" for row in records)
        else:
            check_ctest_records(records, root / "build")
    result, records = invoke(root, env, log, "run_all_tests.sh", empty=True)
    assert "All required development gates passed" not in result.stdout
    check_ctest_records(records, root / "build")
    return configure


def check_sanitizer_contract(root, env, log):
    configurations = []
    for mode, preset, label in (("fast", "minimal", "sanitizer-compatible"),
                                ("full", "dev-gate", None),
                                ("ci-runtime", "dev-gate", "sanitizer-runtime")):
        _, records = invoke(root, env, log, "run_sanitizers.sh", "--" + mode)
        build = root / "build" / "sanitizers" / ("undefined-" + mode)
        configure = configure_command(records, preset, build)
        configurations.append((configure, preset, True, False))
        for option in ("-DCMAKE_BUILD_TYPE=Debug", "-DENABLE_SANITIZERS=ON",
                       "-DLLM_EDGEFLOW_SANITIZERS=undefined",
                       "-DCMAKE_C_COMPILER_LAUNCHER=ccache", "-DCMAKE_CXX_COMPILER_LAUNCHER=ccache"):
            assert option in configure, (option, configure)
        commands = [row["command"] for row in records]
        assert ["ccache", "--zero-stats"] in commands
        assert ["ccache", "--show-stats"] in commands
        assert all(row["ccache_dir"] == env["CCACHE_DIR"] for row in records)
        test = check_ctest_records(records, build)
        assert (test[test.index("-L") + 1] if "-L" in test else None) == label
        build_command = next(command for command in commands if command[:2] == ["cmake", "--build"])
        assert ("edgeflow_dev_tests" in build_command) == (mode == "fast")
        assert ("--target" in build_command) == (mode != "full"), build_command
        if mode == "ci-runtime":
            for target in ("alg_demo", "edgeflow_test_core_runner", "edgeflow_test_nodes_runner",
                           "edgeflow_test_adapter_runner", "edgeflow_test_tooling_runner",
                           "test_cpp_operator_sdk", "test_registry_conflict",
                           "test_model_backend_registry_conflict", "test_catalog_contract_ssot"):
                assert target in build_command, (target, build_command)
            for target in ("alg_pipeline_tool", "alg_pipeline_tool_test", "alg_show"):
                assert target not in build_command, (target, build_command)
        _, failed = invoke(root, env, log, "run_sanitizers.sh", "--" + mode, failure="ctest")
        assert failed[-1]["command"] == ["ccache", "--show-stats"]
        check_ctest_records(failed, build)
        _, empty = invoke(root, env, log, "run_sanitizers.sh", "--" + mode, empty=True)
        check_ctest_records(empty, build)

    override = root / "custom sanitizer build"
    _, records = invoke(root, {**env, "LLM_EDGEFLOW_SANITIZER_BUILD_DIR": str(override)},
                        log, "run_sanitizers.sh", "--fast")
    configure_command(records, "minimal", override)
    if os.uname().sysname == "Linux":
        _, records = invoke(root, {**env, "LLM_EDGEFLOW_SANITIZERS": "thread"},
                            log, "run_sanitizers.sh", "--fast")
        check_ctest_records(records, root / "build/sanitizers/thread-fast")
        assert sum(row["command"][0] == "setarch" for row in records) == 1, records
        assert all(row["arch_wrapped"] == "1" for row in records if row["command"][0] == "ctest")
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    assert "CCACHE_DIR: ${{ github.workspace }}/build/.ccache-sanitizers" in workflow
    assert "key: sanitizer-ccache-v2-" in workflow
    return configurations


def check_real_model_contract(root, env, log):
    configurations = []
    for mode, preset, profiles in (("gguf-only", "dev-gate", ["entity_extract_cpu"]),
                                   ("whisper", "default-cpu", ["audio_asr_intent_cpu"]),
                                   ("all", "default-cpu", ["audio_asr_intent_cpu", "entity_extract_cpu"])):
        _, records = invoke(root, env, log, "run_real_model_e2e.sh", "--" + mode)
        build = root / "build" / "real-models" / mode
        configure = configure_command(records, preset, build)
        configurations.append((configure, preset, False, True))
        assert "-DENABLE_REAL_MODEL_TESTS=ON" in configure
        commands = [row["command"] for row in records]
        fetch_mode = "--whisper-e2e" if mode == "whisper" else "--" + mode
        assert commands[0] == ["fetch_real_test_models.sh", fetch_mode]
        for row in records:
            if row["command"][0] in ("test_real_models_e2e", "alg_demo"):
                assert row["executable"] == str(build / row["command"][0]), row
        assert [command[2] for command in commands if command[:2] == ["alg_demo", "--profile"]] == profiles
        test = next(command for command in commands if command[0] == "test_real_models_e2e")
        expected_filter = {"gguf-only": "--gtest_filter=-RealModelE2ETest.RealWhisperAsrTranscribe",
                           "whisper": "--gtest_filter=RealModelE2ETest.RealWhisperAsrTranscribe"}.get(mode)
        assert [arg for arg in test if arg.startswith("--gtest_filter=")] == ([expected_filter] if expected_filter else [])
    for failure in ("fetch_real_test_models.sh", "configure", "build", "test_real_models_e2e", "alg_demo"):
        _, records = invoke(root, env, log, "run_real_model_e2e.sh", "--all", failure=failure)
        commands = [row["command"] for row in records]
        if failure in ("fetch_real_test_models.sh", "configure", "build"):
            assert all(command[0] not in ("test_real_models_e2e", "alg_demo") for command in commands)
        if failure == "test_real_models_e2e":
            assert not any(command[0] == "alg_demo" for command in commands)
    return configurations


def cache_values(build):
    return {line.split("=", 1)[0].split(":", 1)[0]: line.split("=", 1)[1]
            for line in (build / "CMakeCache.txt").read_text().splitlines()
            if "=" in line and not line.startswith(("#", "//"))}


def check_cache_reset(root, configurations):
    """Apply the actual script commands and shared profiles to stale CMake caches."""
    fixture = root / "fixture"
    fixture.mkdir()
    shutil.copy2(ROOT / "CMakePresets.json", fixture / "CMakePresets.json")
    (fixture / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.19)\nproject(GateFixture NONE)\n'
        'include(CTest)\nif(BUILD_TESTING)\n'
        'add_test(NAME required COMMAND "${CMAKE_COMMAND}" -E touch "${CMAKE_BINARY_DIR}/executed")\n'
        'set_tests_properties(required PROPERTIES LABELS fixture)\nendif()\n')
    cmake, ctest = shutil.which("cmake"), shutil.which("ctest")
    build = root / "fixture-build"
    generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
    backend_flags = {"minimal": (), "dev-gate": ("ONNXRUNTIME", "LLAMACPP"),
                     "default-cpu": ("ONNXRUNTIME", "LLAMACPP", "WHISPERCPP"),
                     "kite-cpu": ("ONNXRUNTIME", "KITELLM")}
    # Also check every supported direct profile, including the Kite build.
    configurations += [(None, preset, False, False) for preset in backend_flags]
    for command, preset, sanitizer, real_models in configurations:
        expected = {"BUILD_TESTING": "ON", "ENABLE_SANITIZERS": "ON" if sanitizer else "OFF",
                    "ENABLE_REAL_MODEL_TESTS": "ON" if real_models else "OFF",
                    "LLM_EDGEFLOW_TEST_PCH": "OFF",
                    "CMAKE_BUILD_TYPE": "Debug" if sanitizer else "Release"}
        expected.update({"ENABLE_" + flag: "ON" if flag in backend_flags[preset] else "OFF"
                         for flag in ("ONNXRUNTIME", "LLAMACPP", "WHISPERCPP", "KITELLM")})
        stale = ["-D" + key + "=" + ("OFF" if value == "ON" else "ON")
                 for key, value in expected.items() if key != "CMAKE_BUILD_TYPE"]
        result = run([cmake, "-S", str(fixture), "-B", str(build), "-G", generator, *stale, "-DCMAKE_BUILD_TYPE=RelWithDebInfo"])
        assert result.returncode == 0, result.stdout + result.stderr
        if command is None:
            # Exercise profile resolution from outside the fixture's source cwd.
            actual = [str(ROOT / "scripts/configure_build.sh"), str(fixture), str(build), preset]
        else:
            actual = [cmake, *command[1:]]
            actual[actual.index("-S") + 1] = str(fixture)
            actual[actual.index("-B") + 1] = str(build)
            actual[actual.index("-G") + 1] = generator
        result = run(actual, cwd=root)
        assert result.returncode == 0, result.stdout + result.stderr
        values = cache_values(build)
        assert {key: values[key] for key in expected} == expected, (preset, command, values)
        inventory = run([ctest, "--show-only=json-v1"], cwd=build)
        assert inventory.returncode == 0, inventory.stdout + inventory.stderr
        assert [test["name"] for test in json.loads(inventory.stdout)["tests"]] == ["required"]
    result = run([ctest, "-L", "^fixture$", "-R", "^required$", "--output-on-failure", "--no-tests=error"], cwd=build)
    assert result.returncode == 0, result.stdout + result.stderr
    marker = build / "executed"
    assert marker.exists(), "CTest did not execute its selected test"
    marker.unlink()
    for filters in (("-L", "missing-label"), ("-R", "missing-test"), ("-E", "required")):
        result = run([ctest, *filters, "--no-tests=error"], cwd=build)
        assert result.returncode != 0, (filters, result.stdout, result.stderr)
        assert not marker.exists(), (filters, "An empty filtered inventory executed tests")


def main():
    with tempfile.TemporaryDirectory(prefix="edgeflow-quality-contract-") as directory:
        root = Path(directory)
        env, log = mock_project(root)
        gate = check_gate_contract(root, env, log)
        configurations = [(gate, "dev-gate", False, False)]
        configurations += check_sanitizer_contract(root, env, log)
        configurations += check_real_model_contract(root, env, log)
        check_cache_reset(root, configurations)
        workflow = (ROOT / ".github/workflows/ci.yml").read_text()
        assert "WHISPER_GATE_RESULT: ${{ needs.whisper-asr.result }}" in workflow
        assert "KITELLM_GATE_RESULT: ${{ needs.kite-llm.result }}" in workflow
        assert "run: ./scripts/fetch_real_test_models.sh --gguf-only" not in workflow
        assert "run: ./scripts/fetch_real_test_models.sh --whisper" not in workflow
        # Each ccache job falls back to its own main snapshot before any shared prefix, and
        # every restored ccache is saved through the pruning action.
        for prefix in ("ccache-real-", "ccache-whisper-"):
            assert f"            {prefix}${{{{ runner.os }}}}-\n" in workflow, prefix
        assert "uses: actions/cache@v4\n        with:\n          path: ${{ env.CCACHE_DIR }}" \
            not in workflow
        assert (workflow.count("uses: ./.github/actions/ccache-restore")
                == workflow.count("uses: ./.github/actions/ccache-save") == 5)
        manifest = json.loads((ROOT / "models/asset_manifest.json").read_text())
        artifact_groups = {
            group: {name for name, artifact in manifest["artifacts"].items()
                    if group in artifact.get("download_groups", [])}
            for group in ("whisper", "whisper-e2e")
        }
        assert artifact_groups["whisper-e2e"] == {"ggml-base.bin"}
        assert artifact_groups["whisper"] == {"ggml-base.bin", "ggml-tiny-q5_1.bin"}
        evidence = root / "evidence.json"
        for state in ("success", "failure", "skipped", "cancelled"):
            evidence_env = {**os.environ, "WHISPER_GATE_RESULT": state,
                            "KITELLM_GATE_RESULT": "skipped"}
            result = run([str(root / "scripts/generate_acceptance_evidence.sh"), str(evidence),
                          "success", "failure", "cancelled"], env=evidence_env)
            assert result.returncode == 0, result.stdout + result.stderr
            report = json.loads(evidence.read_text())
            assert report["schema_version"] == 3
            gates = report["gates"]
            assert gates == {"canonical_run_all_tests": "success",
                             "ci_runtime_address_undefined_sanitizer": "failure",
                             "real_c_abi_and_public_profile": "cancelled",
                             "whisper_asr_backend_and_real_profile": state,
                             "kitellm_private_release_and_real_gguf": "skipped"}
        check_delivery_contract(root)
        check_delivery_history_contract(root)
    print("Build presets, stale cache resets, gate failures, real-model routing and delivery checks passed.")


if __name__ == "__main__":
    main()
