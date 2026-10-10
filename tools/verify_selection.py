#!/usr/bin/env python3
"""Verify a selection against executable Catalog, pinned assets and measured effects.

No inference runtime or Catalog is reimplemented here. The native validator is
always the configuration authority. Effect scores are exact matches of selected
JSON fields, scoped to an explicit labelled dataset, never general model quality.
"""
import argparse
import copy
import datetime
import hashlib
import json
import platform
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "configs/asset_manifest.json"
PRESETS = ROOT / "CMakePresets.json"


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def file_digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def pointer(document, path):
    if not path.startswith("/"):
        raise ValueError("Expected a JSON pointer: " + path)
    value = document
    for part in path[1:].split("/"):
        key = part.replace("~1", "/").replace("~0", "~")
        value = value[int(key)] if isinstance(value, list) else value[key]
    return value


def native(tool, command, document=None):
    args = [str(Path(tool).resolve()), *(command if isinstance(command, list) else [command])]
    if document is not None:
        args.append("--stdin")
    result = subprocess.run(args, input=json.dumps(document) if document is not None else None,
                            text=True, capture_output=True, timeout=60, check=False)
    payload = json.loads(result.stdout)
    if result.returncode not in (0, 1):
        raise ValueError("Native tool failed: " + result.stderr[-1000:])
    return payload


def within(root, relative):
    root = Path(root).resolve()
    path = (root / relative).resolve()
    if path != root and root not in path.parents:
        raise ValueError("Asset path leaves pipeline directory: " + str(relative))
    return path


def pipeline_io(pipeline):
    if not isinstance(pipeline, dict):
        return [], []
    io = pipeline.get("io", {})
    return (io.get("input", []), io.get("output", [])) if isinstance(io, dict) else ([], [])


def build_run_conf(pipeline, outputs, pipe_path):
    """Apply explicit output parameters without changing resource paths."""
    selected = pipeline_io(pipeline)[1]
    for output in outputs:
        matches = [entry for entry in selected
                   if (entry["type"], entry["name"]) == (output["type"], output["name"])]
        if len(matches) != 1:
            raise ValueError("Output parameter override must match one selected converter")
        if output.get("params"):
            matches[0].setdefault("params", {}).update(copy.deepcopy(output["params"]))
    return {"pipe_path": Path(pipe_path).name}


def validate_manifest(manifest):
    if not isinstance(manifest, dict):
        raise ValueError("Asset manifest must be a JSON object")
    artifacts = manifest["artifacts"]
    for name, artifact in artifacts.items():
        if Path(name).is_absolute() or ".." in Path(name).parts or not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]):
            raise ValueError("Invalid asset path or SHA-256: " + name)
    ids = set()
    for choice in manifest["selections"]:
        files, paths = set(choice["files"]), choice["paths"]
        if (choice["id"] in ids or not files or "/file" not in paths or
                not set(paths.values()).issubset(files) or not files.issubset(artifacts)):
            raise ValueError("Incomplete or duplicate asset selection: " + choice["id"])
        for path, name in paths.items():
            if pointer(choice["model"], path) != name:
                raise ValueError("Asset selection template and path declarations disagree")
        ids.add(choice["id"])
    return manifest


def build_variants():
    # 仅用于构建的 preset 和隐藏的继承默认值不属于资源选型。
    return [item for item in read_json(PRESETS)["configurePresets"]
            if not item.get("hidden", False)
            and "llm-edgeflow/selection" in item.get("vendor", {})]


def asset_catalog(manifest=MANIFEST, pipeline_dir=ROOT / "configs"):
    catalog = validate_manifest(read_json(manifest))
    for selection in catalog["selections"]:
        selection["availability"] = "unverified" if pipeline_dir is None else (
            "present_unverified" if all(within(pipeline_dir, name).is_file()
                                        for name in selection["files"]) else "missing")
    catalog["variants"] = [{"name": item["name"], **item["vendor"]["llm-edgeflow/selection"]}
                           for item in build_variants()]
    return catalog


def model_asset_path(model, field, pipeline_dir):
    return within(pipeline_dir, pointer(model, field))


def verify_assets(pipeline, pipeline_dir, manifest):
    validate_manifest(manifest)
    checked = []
    hash_cache = {}
    for model in pipeline.get("models", []):
        row = {"name": model["name"], "status": "unregistered", "files": []}
        if pipeline_dir is None:
            row["status"] = "unverified"
            checked.append(row)
            continue
        choices = [item for item in manifest["selections"]
                   if item["model"]["type"] == model["type"]
                   and item["model"]["backend"]["type"] == model["backend"]["type"]]
        choice = None
        for item in choices:
            try:
                if all(model_asset_path(model, key, pipeline_dir) == within(pipeline_dir, value)
                       for key, value in item["paths"].items()):
                    choice = item
                    break
            except (KeyError, ValueError):
                continue
        if choice:
            row["asset_id"] = choice["id"]
            for name in choice["files"]:
                expected = manifest["artifacts"][name]["sha256"]
                path = within(pipeline_dir, name)
                if path not in hash_cache:
                    hash_cache[path] = file_digest(path) if path.is_file() else None
                actual = hash_cache[path]
                row["files"].append({"path": str(path), "expected_sha256": expected,
                                     "actual_sha256": actual,
                                     "status": "verified" if actual == expected else
                                               "missing" if actual is None else "hash_mismatch"})
            row["status"] = "verified" if all(f["status"] == "verified" for f in row["files"]) else "failed"
        checked.append(row)
    return checked


def inspect_selection(pipeline, tool, pipeline_dir, manifest=MANIFEST, variant=None):
    catalog = native(tool, "catalog")
    if pipeline_dir is None:
        validation = native(tool, "validate", pipeline)
    else:
        pipeline_dir = Path(pipeline_dir).resolve()
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".selection-",
                                         suffix=".json", dir=pipeline_dir) as temporary:
            json.dump(pipeline, temporary, ensure_ascii=False)
            temporary.flush()
            validation = native(tool, ["validate", temporary.name])
    assets = verify_assets(pipeline, pipeline_dir, read_json(manifest)) if validation.get("ok") else []
    enabled = sorted(item["backend_type"] for item in catalog["backends"])
    build = {"enabled_backends": enabled, "tool_sha256": file_digest(tool),
             "platform": platform.platform(), "variant": variant, "status": "catalog_verified"}
    if variant:
        preset = next((p for p in build_variants() if p["name"] == variant), None)
        if not preset:
            raise ValueError("Unknown build variant: " + variant)
        expected = sorted(preset["vendor"]["llm-edgeflow/selection"]["backends"])
        build.update(expected_backends=expected, status="verified" if enabled == expected else "variant_mismatch")
    ok = validation.get("ok", False) and all(item["status"] == "verified" for item in assets) and build["status"] != "variant_mismatch"
    fingerprint = digest({"pipeline": pipeline, "pipeline_dir": str(pipeline_dir) if pipeline_dir else None,
                          "assets": assets, "build": build})
    return {"ok": bool(ok), "selection_fingerprint": fingerprint,
            "configuration": validation, "build": build, "models": assets,
            "effects": {"status": "unverified"}, "ready_for_biz": False}


def compare_samples(records, spec):
    expected = spec.get("samples", [])
    if not expected or any(not row.get("expected") for row in expected):
        raise ValueError("An effect specification needs nonempty labelled samples and checks")
    if len(records) != len(expected):
        return {"status": "failed", "reason": "Unexpected result count", "pass_rate": 0.0}
    passed = 0
    failures = []
    for row, (record, expectation) in enumerate(zip(records, expected)):
        matches = record.get("status") == 0
        for path, value in expectation["expected"].items():
            try:
                matches = matches and pointer(record, path) == value
            except (KeyError, IndexError, TypeError):
                matches = False
        if matches:
            passed += 1
        else:
            failures.append(row)
    rate = passed / len(expected)
    minimum = spec.get("minimum_pass_rate", 1.0)
    if not isinstance(minimum, (int, float)) or not 0 < minimum <= 1:
        raise ValueError("minimum_pass_rate must be in (0, 1]")
    return {"status": "passed" if rate >= minimum else "failed", "metric": "selected_fields_exact_match",
            "total": len(expected), "passed": passed, "pass_rate": rate, "minimum_pass_rate": minimum,
            "failed_rows": failures}


def effect_output(pipeline, spec):
    output = spec.get("output")
    if not isinstance(output, dict) or set(output) != {"type", "name"}:
        raise ValueError("Effect specification needs an output converter type/name pair")
    matches = [entry for entry in pipeline_io(pipeline)[1]
               if (entry["type"], entry["name"]) == (output["type"], output["name"])]
    if len(matches) != 1:
        raise ValueError("Effect specification must match exactly one selected output converter")
    return matches[0]


def effect_inputs(spec_path, conf_path, demo, pipeline_dir):
    spec_path = Path(spec_path).resolve()
    spec = read_json(spec_path)
    dataset = (spec_path.parent / spec["dataset"]).resolve()
    conf = read_json(conf_path)
    if (not isinstance(conf, dict) or set(conf) != {"pipe_path"}
            or not isinstance(conf["pipe_path"], str) or not conf["pipe_path"].strip()):
        raise ValueError(f"Conf must contain only non-empty 'pipe_path': {conf_path}")
    pipe_path = conf["pipe_path"]
    pipeline_file = (Path(conf_path).parent / pipe_path).resolve()
    pipeline_dir = Path(pipeline_dir).resolve()
    if pipeline_file.parent != pipeline_dir:
        raise ValueError("Conf pipeline is outside the selected pipeline directory")
    pipe_doc = read_json(pipeline_file)
    effect_output(pipe_doc, spec)
    inputs, outputs = pipeline_io(pipe_doc)
    demo = Path(demo).resolve()
    sdk = demo.parent / "libcompany_alg_sdk.so"
    identity = {"spec": spec, "dataset_sha256": file_digest(dataset), "outputs": outputs,
                "pipeline": pipe_doc, "pipeline_dir": str(pipeline_dir), "inputs": inputs,
                "demo_sha256": file_digest(demo), "sdk": {sdk.name: file_digest(sdk)} if sdk.is_file() else {},
                "chip": "cpu", "device_id": 0}
    return spec, dataset, outputs, identity


def evaluate(pipeline, selection, tool, pipeline_dir, spec_path, conf_path, demo):
    if not selection["ok"]:
        raise ValueError("Configuration, build or assets are not verified")
    spec, dataset, outputs, test_inputs = effect_inputs(spec_path, conf_path, demo, pipeline_dir)
    test_fingerprint = digest(test_inputs)
    effect_output(pipeline, spec)
    pipeline_dir = Path(pipeline_dir).resolve()
    execution_pipeline = copy.deepcopy(pipeline)
    # The temporary documents share the original directory so relative resources stay valid.
    with tempfile.TemporaryDirectory(prefix="edgeflow-selection-results-") as directory, \
            tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".selection-",
                                        suffix=".json", dir=pipeline_dir) as temporary_pipeline, \
            tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", prefix=".selection-",
                                        suffix=".conf", dir=pipeline_dir) as temporary_conf:
        conf_file = Path(temporary_conf.name)
        generated_conf = build_run_conf(execution_pipeline, outputs, temporary_pipeline.name)
        json.dump(execution_pipeline, temporary_pipeline, ensure_ascii=False)
        temporary_pipeline.flush()
        json.dump(generated_conf, temporary_conf)
        temporary_conf.flush()
        resolved = native(tool, ["resolve-conf", conf_file.name, "--root", str(pipeline_dir)])
        if not resolved.get("ok"):
            raise ValueError("Effect deployment is invalid: " + json.dumps(resolved))
        results = Path(directory)
        command = [str(Path(demo).resolve()), "--config", conf_file.name,
                   "--dataset", str(dataset), "--output-dir", str(results)]
        process = subprocess.run(command, cwd=pipeline_dir, text=True, capture_output=True, timeout=1800, check=False)
        if process.returncode:
            raise ValueError("Effect run failed: " + (process.stdout + process.stderr)[-3000:])
        records = [json.loads(line) for line in (results / conf_file.stem / "results.jsonl").read_text().splitlines() if line.strip()]
    if digest(effect_inputs(spec_path, conf_path, demo, pipeline_dir)[3]) != test_fingerprint:
        raise ValueError("Effect inputs or binaries changed during execution")
    metrics = compare_samples(records, spec)
    return {"generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "selection_fingerprint": selection["selection_fingerprint"], "test_fingerprint": test_fingerprint,
            "pipeline": execution_pipeline, "selection": selection, "test_inputs": test_inputs,
            "metrics": metrics, "records": records}


def attach_evidence(selection, evidence_path, spec_path, conf_path, demo, pipeline_dir):
    evidence = read_json(evidence_path)
    spec, _, _, identity = effect_inputs(spec_path, conf_path, demo, pipeline_dir)
    if (evidence.get("selection_fingerprint") != selection["selection_fingerprint"] or
            evidence.get("test_fingerprint") != digest(identity)):
        selection["effects"] = {"status": "stale", "reason": "Selection, dataset, criteria, deployment or executable changed"}
    else:
        selection["effects"] = compare_samples(evidence["records"], spec)
        selection["effects"]["generated_at_utc"] = evidence.get("generated_at_utc")
    selection["ready_for_biz"] = selection["ok"] and selection["effects"]["status"] == "passed"
    return selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check", "evaluate"])
    parser.add_argument("--pipeline", required=True, type=Path)
    parser.add_argument("--tool", type=Path, default=ROOT / "build/alg_pipeline_tool")
    parser.add_argument("--demo", type=Path, default=ROOT / "build/alg_demo")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--variant")
    parser.add_argument("--effects", type=Path)
    parser.add_argument("--conf", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-effects", action="store_true")
    args = parser.parse_args()
    try:
        pipeline = read_json(args.pipeline)
        pipeline_dir = args.pipeline.resolve().parent
        report = inspect_selection(pipeline, args.tool, pipeline_dir, args.manifest, args.variant)
        conf = args.conf or args.pipeline.with_suffix(".conf")
        if args.command == "evaluate":
            if not args.effects or not args.output:
                parser.error("evaluate requires --effects and --output")
            report = evaluate(pipeline, report, args.tool, pipeline_dir, args.effects, conf, args.demo)
            # 推理后重新计算资源哈希，以检测运行期间的变更。
            if inspect_selection(pipeline, args.tool, pipeline_dir, args.manifest, args.variant)["selection_fingerprint"] != report["selection_fingerprint"]:
                raise ValueError("Selection assets changed during execution")
            ok = report["metrics"]["status"] == "passed"
        else:
            if args.evidence:
                if not args.effects:
                    parser.error("--evidence requires --effects")
                attach_evidence(report, args.evidence, args.effects, conf, args.demo, pipeline_dir)
            ok = report["ready_for_biz"] if args.require_effects else report["ok"]
        encoded = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(encoded)
        print(encoded, end="")
        return 0 if ok else 1
    except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"ok": False, "error": str(error)}, ensure_ascii=False))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
