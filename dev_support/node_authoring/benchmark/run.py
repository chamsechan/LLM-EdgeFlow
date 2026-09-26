#!/usr/bin/env python3
"""Compare Node wrappers on the same runtime, without real models.

Requires the repository's pinned dependency caches. Generated sources and results
stay beside the selected build directory. Counts C++ new/new[] requests, not
malloc, retained heap, or peak RSS. The baseline starter is read from Git.
"""

import argparse
import hashlib
import json
import os
import platform
from pathlib import Path
import shlex
import shutil
import statistics
import subprocess
import time


BASELINE_HELPER_REVISION = "87a28b7"

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--baseline", default="87f490b")
    parser.add_argument("--iterations", type=int, default=10000)
    args = parser.parse_args()
    if args.iterations < 1:
        parser.error("--iterations must be positive")
    root, build = args.source.resolve(), args.build.resolve()
    work = build.with_name(build.name + "-source")
    work.mkdir(parents=True, exist_ok=True)
    for name in ("probe.cpp", "CMakeLists.txt"):
        shutil.copyfile(Path(__file__).with_name(name), work / name)
    cache = work / "3rdparty"
    if not cache.exists():
        cache.symlink_to(root / "3rdparty", target_is_directory=True)
    env = dict(os.environ, CCACHE_DISABLE="1")

    def run(command, **kwargs):
        return subprocess.run(command, check=True, text=True, env=env, **kwargs)

    baseline_headers = work / "baseline_include" / "nodes"
    baseline_headers.mkdir(parents=True, exist_ok=True)
    for name in ("model_bound_node.h", "node_definition_helpers.h"):
        header = run(["git", "-C", str(root), "show",
                      BASELINE_HELPER_REVISION + ":include/nodes/" + name],
                     capture_output=True).stdout
        (baseline_headers / name).write_text(header)

    old = run(["git", "-C", str(root), "show",
               args.baseline + ":dev_support/node_authoring/starter_llm_node.cpp"],
              capture_output=True).stdout
    (work / "baseline_starter.cpp").write_text(old.replace("StarterLlmNode", "BaselineStarterLlmNode"))
    current = (root / "dev_support/node_authoring/starter_llm_node.cpp").read_text()
    (work / "current_starter.cpp").write_text(current.replace("StarterLlmNode", "ProbeStarterLlmNode"))
    run(["cmake", "-S", str(work), "-B", str(build), "-DEDGEFLOW_SOURCE=" + str(root),
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"])
    run(["cmake", "--build", str(build), "--target", "node_authoring_probe", "-j8"])
    modes = ("old_map", "new_map", "old_llm", "new_llm", "new_batch", "new_batch_inplace")
    results = []
    for repeat in range(5):
        for mode in modes:
            output = run([str(build / "node_authoring_probe"), mode, str(args.iterations)], capture_output=True)
            row = json.loads(output.stdout.strip().splitlines()[-1])
            results.append(dict(row, repeat=repeat + 1))
    (work / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    summaries = {}
    for mode in modes:
        rows = [row for row in results if row["mode"] == mode]
        values = [row["cpu_us_per_process"] for row in rows]
        summaries[mode] = dict(cpu_us_median=statistics.median(values),
                               cpu_us_min=min(values), cpu_us_max=max(values),
                               new_calls=rows[0]["new_calls_per_process"],
                               new_bytes=rows[0]["new_bytes_per_process"],
                               model_calls=rows[0]["model_calls_per_process"])
    commands = json.loads((build / "compile_commands.json").read_text())
    entry = next(item for item in commands if Path(item["file"]).name == "probe.cpp")
    base = entry.get("arguments") or shlex.split(entry["command"])
    output_index = base.index("-o") + 1
    source_index = base.index(entry["file"])
    compile_results = []
    for repeat in range(5):
        for label, source in (("old_llm", "baseline_starter.cpp"), ("new_llm", "current_starter.cpp")):
            command = base.copy()
            command[output_index] = str(build / (label + ".o"))
            command[source_index] = str(work / source)
            start = time.perf_counter()
            run(command, cwd=entry["directory"], capture_output=True)
            compile_results.append(dict(repeat=repeat + 1, mode=label, wall_seconds=time.perf_counter() - start))
    hashes = {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in (
        "include/nodes/function_node.h", "include/nodes/parameter_binding.h",
        "include/nodes/model_calls.h", "dev_support/node_authoring/starter_llm_node.cpp")}
    summary = dict(baseline_revision=args.baseline,
                   baseline_helper_revision=BASELINE_HELPER_REVISION,
                   platform=platform.platform(),
                   compile_command=base, iterations=args.iterations, source_sha256=hashes,
                   wrappers=summaries, compile_results=compile_results)
    (work / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print("Raw measurements:", work / "results.json")


if __name__ == "__main__":
    main()
