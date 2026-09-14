#!/usr/bin/env python3
"""Compare RFC-0054 node implementations on an otherwise idle machine.

Run the canonical gate/build first. This script reuses its Ninja node-runner
runtime objects and libraries without building the repository. Only the two node
translation units are replaced for the baseline; this is not a full historical
checkout benchmark. Both versions use C++17, -O3 and -DNDEBUG. Each invocation
processes 2,000 requests of 50 samples; writer updates are spaced by 100 us.
Ordinary C++ allocation counts are measured separately from request timing.
"""

import argparse
import json
from pathlib import Path
import shlex
import statistics
import subprocess
import sys


NODES = ("text_template_node", "text_rule_match_node")
ROOT = Path(__file__).resolve().parents[2]


def positive_integer(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be at least 1")
    return number


def runner_link_command(commands):
    """Keep compiler/launcher arguments while stripping Ninja shell wrappers."""
    for line in reversed(commands.splitlines()):
        tokens = shlex.split(line)
        if "-o" not in tokens or not all(
            any(token.endswith(f"/{node}.cpp.o") for token in tokens)
            for node in NODES
        ):
            continue
        if tokens[:2] == [":", "&&"]:
            tokens = tokens[2:]
        if tokens[-2:] == ["&&", ":"]:
            tokens = tokens[:-2]
        if any(token in ("&&", ";", "|") for token in tokens):
            raise RuntimeError("Unsupported shell wrapper in the node-runner link command")
        return [
            token for token in tokens
            if not token.startswith("tests/CMakeFiles/")
            and not (
                token.endswith(".o")
                and "edgeflow_test_allocation_failure" in token
            )
        ]
    raise RuntimeError("Cannot find the Ninja node-runner link command; run the gate/build first")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="empty temporary directory for binaries and all evidence")
    parser.add_argument("--baseline", default="7a6ca02")
    parser.add_argument("--rounds", type=positive_integer, default=7)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    if not (build / "build.ninja").is_file():
        parser.error("Ninja build directory is missing; run ./scripts/run_all_tests.sh first")
    if output.exists() and any(output.iterdir()):
        parser.error("--output-dir must be empty to preserve previous evidence")
    output.mkdir(parents=True, exist_ok=True)
    print("Run only with an idle machine; compiling isolated benchmark objects.", flush=True)

    with (output / "commands.log").open("w") as log:
        def run(command, cwd=ROOT):
            command = [str(token) for token in command]
            log.write(f"cwd={cwd}\n{shlex.join(command)}\n")
            log.flush()
            result = subprocess.run(command, cwd=cwd, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            log.write(result.stdout)
            log.flush()
            if result.returncode:
                raise RuntimeError(
                    f"Command failed ({result.returncode}): {shlex.join(command)}; "
                    f"see {output / 'commands.log'}. Ensure the gate/build completed first."
                )
            return result.stdout

        environment = "".join(run(command) for command in (
            ["uname", "-a"], ["c++", "--version"], ["lscpu"],
            ["git", "rev-parse", "HEAD"], ["git", "status", "--short"],
            ["git", "rev-parse", args.baseline],
        ))
        environment += f"\narguments: {vars(args)}\n"
        (output / "environment.txt").write_text(environment)
        link = runner_link_command(run([
            "ninja", "-C", build, "-t", "commands", "edgeflow_test_nodes_runner"
        ]))
        flags = ["c++", "-O3", "-DNDEBUG", "-std=c++17", "-fPIC", "-fopenmp"]
        flags.extend(f"-I{path}" for path in (
            build / "layer_includes/capability_nodes", ROOT / "include", ROOT,
            build / "generated/include", ROOT / "3rdparty/nlohmann_json/include",
        ))
        benchmark_object = output / "bench.o"
        run(flags + ["-c", Path(__file__).with_suffix(".cpp"), "-o", benchmark_object])
        for version in ("baseline", "current"):
            objects = []
            for node in NODES:
                relative_source = f"src/common_nodes/{node}.cpp"
                source = output / f"{version}_{node}.cpp"
                source.write_text(
                    run(["git", "show", f"{args.baseline}:{relative_source}"])
                    if version == "baseline" else (ROOT / relative_source).read_text()
                )
                obj = output / f"{version}_{node}.o"
                run(flags + ["-c", source, "-o", obj])
                objects.append(str(obj))
            command = [token for token in link if not any(
                token.endswith(f"/{node}.cpp.o") for node in NODES
            )]
            command[command.index("-o") + 1] = str(output / f"bench_{version}")
            # Put replacement objects ahead of static libraries for normal linkers.
            command[command.index("-o"):command.index("-o")] = objects + [str(benchmark_object)]
            run(command, cwd=build)

        records = []
        for round_index in range(args.rounds):
            for node in ("template", "rules"):
                for concurrent in (0, 1):
                    versions = ("baseline", "current") if round_index % 2 == 0 else ("current", "baseline")
                    for version in versions:
                        stdout = run([output / f"bench_{version}", node, concurrent])
                        (output / f"{round_index}_{node}_{concurrent}_{version}.log").write_text(stdout)
                        result = next(line.split() for line in stdout.splitlines() if line.startswith("RESULT "))
                        allocation = next(line.split() for line in stdout.splitlines() if line.startswith("ALLOC "))
                        records.append(dict(
                            round=round_index, node=node, concurrent=concurrent, version=version,
                            us=float(result[3]), updates=int(result[4]),
                            allocations=int(allocation[1]), bytes=int(allocation[2]),
                        ))
                        (output / "results.json").write_text(json.dumps(records, indent=2) + "\n")
            print(f"Completed round {round_index + 1}/{args.rounds}", flush=True)

        summary = []
        for node in ("template", "rules"):
            for concurrent in (0, 1):
                medians = {version: statistics.median(
                    record["us"] for record in records
                    if record["node"] == node and record["concurrent"] == concurrent
                    and record["version"] == version
                ) for version in ("baseline", "current")}
                change = 100 * (medians["current"] / medians["baseline"] - 1)
                summary.append(f"{node} concurrent={concurrent}: {medians}, change_pct={change}\n")
        (output / "summary.txt").write_text("".join(summary))
        print("".join(summary), end="")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, StopIteration, ValueError) as error:
        print(f"Benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
