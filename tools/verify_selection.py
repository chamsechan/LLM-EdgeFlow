#!/usr/bin/env python3
"""Verify a selection against executable Catalog, pinned assets and measured effects.

No inference runtime or Catalog is reimplemented here. The native validator is
always the configuration authority. Effect scores are exact matches of selected
JSON fields, scoped to an explicit labelled dataset, never general model quality.
"""
import argparse
import datetime
import hashlib
import json
import platform
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "models/asset_manifest.json"
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
    args = [str(Path(tool).resolve()), command]
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
        raise ValueError("Asset path leaves model root: " + str(relative))
    return path


def validate_manifest(manifest):
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1:
        raise ValueError("Unsupported asset manifest version")
    artifacts = manifest["artifacts"]
    for name, artifact in artifacts.items():
        if Path(name).is_absolute() or ".." in Path(name).parts or not re.fullmatch(r"[0-9a-f]{64}", artifact["sha256"]):
            raise ValueError("Invalid asset path or SHA-256: " + name)
    ids = set()
    for choice in manifest["selections"]:
        files, paths = set(choice["files"]), choice["paths"]
        if (choice["id"] in ids or not files or "/model_path" not in paths or
                not set(paths.values()).issubset(files) or not files.issubset(artifacts)):
            raise ValueError("Incomplete or duplicate asset selection: " + choice["id"])
        for path, name in paths.items():
            if pointer(choice["model"], path) != name:
                raise ValueError("Asset selection template and path declarations disagree")
        ids.add(choice["id"])
    return manifest


def asset_catalog(manifest=MANIFEST, model_root=ROOT / "models"):
    catalog = validate_manifest(read_json(manifest))
    for selection in catalog["selections"]:
        selection["availability"] = "present_unverified" if all(
            within(model_root, name).is_file() for name in selection["files"]) else "missing"
    catalog["variants"] = [{"name": item["name"], **item["vendor"]["llm-edgeflow/selection"]}
                           for item in read_json(PRESETS)["configurePresets"]]
    return catalog


def verify_assets(pipeline, model_root, manifest):
    validate_manifest(manifest)
    checked = []
    hash_cache = {}
    for model in pipeline.get("models", []):
        row = {"model_id": model["model_id"], "status": "unregistered", "files": []}
        choices = [item for item in manifest["selections"]
                   if item["model"]["model_type"] == model["model_type"]
                   and item["model"]["backend"] == model["backend"]]
        choice = None
        for item in choices:
            try:
                if all(within(model_root, pointer(model, key)) == within(model_root, value)
                       for key, value in item["paths"].items()):
                    choice = item
                    break
            except (KeyError, ValueError):
                continue
        if choice:
            row["asset_id"] = choice["id"]
            for name in choice["files"]:
                expected = manifest["artifacts"][name]["sha256"]
                path = within(model_root, name)
                if path not in hash_cache:
                    hash_cache[path] = file_digest(path) if path.is_file() else None
                actual = hash_cache[path]
                row["files"].append({"path": name, "expected_sha256": expected,
                                     "actual_sha256": actual,
                                     "status": "verified" if actual == expected else
                                               "missing" if actual is None else "hash_mismatch"})
            row["status"] = "verified" if all(f["status"] == "verified" for f in row["files"]) else "failed"
        checked.append(row)
    return checked


def inspect_selection(pipeline, tool, model_root, manifest=MANIFEST, variant=None):
    catalog = native(tool, "catalog")
    validation = native(tool, "validate", pipeline)
    assets = verify_assets(pipeline, model_root, read_json(manifest)) if validation.get("ok") else []
    enabled = sorted(item["backend_type"] for item in catalog["backends"])
    build = {"enabled_backends": enabled, "tool_sha256": file_digest(tool),
             "platform": platform.platform(), "variant": variant, "status": "catalog_verified"}
    if variant:
        preset = next((p for p in read_json(PRESETS)["configurePresets"] if p["name"] == variant), None)
        if not preset:
            raise ValueError("Unknown build variant: " + variant)
        expected = sorted(preset["vendor"]["llm-edgeflow/selection"]["backends"])
        build.update(expected_backends=expected, status="verified" if enabled == expected else "variant_mismatch")
    ok = validation.get("ok", False) and all(item["status"] == "verified" for item in assets) and build["status"] != "variant_mismatch"
    fingerprint = digest({"pipeline": pipeline, "assets": assets, "build": build})
    return {"schema_version": 1, "ok": bool(ok), "selection_fingerprint": fingerprint,
            "configuration": validation, "build": build, "models": assets,
            "effects": {"status": "unverified"}, "ready_for_business": False}


def compare_samples(records, spec):
    expected = spec.get("samples", [])
    if not expected or any(not row.get("expected") for row in expected):
        raise ValueError("An effect specification needs nonempty labelled samples and checks")
    desired = {row["request_id"]: row for row in expected}
    actual = {row["request_id"]: row for row in records}
    if len(desired) != len(expected) or len(actual) != len(records) or desired.keys() != actual.keys():
        return {"status": "failed", "reason": "Missing, duplicate or unexpected result request IDs", "pass_rate": 0.0}
    passed = 0
    failures = []
    for req_id, expectation in desired.items():
        record = actual[req_id]
        matches = record.get("status") == 0
        for path, value in expectation["expected"].items():
            try:
                matches = matches and pointer(record, path) == value
            except (KeyError, IndexError, TypeError):
                matches = False
        if matches:
            passed += 1
        else:
            failures.append(req_id)
    rate = passed / len(expected)
    minimum = spec.get("minimum_pass_rate", 1.0)
    if not isinstance(minimum, (int, float)) or not 0 < minimum <= 1:
        raise ValueError("minimum_pass_rate must be in (0, 1]")
    return {"status": "passed" if rate >= minimum else "failed", "metric": "selected_fields_exact_match",
            "total": len(expected), "passed": passed, "pass_rate": rate, "minimum_pass_rate": minimum,
            "failed_request_ids": failures}


def effect_inputs(spec_path, conf_path, demo):
    spec_path = Path(spec_path).resolve()
    spec = read_json(spec_path)
    dataset = (spec_path.parent / spec["dataset"]).resolve()
    conf = read_json(conf_path)
    # The evaluator deliberately regenerates model_paths from the selected
    # Pipeline; only deployment output capacities are inherited.
    mem_que = conf["data"]["mem_que"]
    demo = Path(demo).resolve()
    sdk_candidates = list(demo.parent.glob("libcompany_alg_sdk.*"))
    sdk_files = sorted({path.resolve() for path in sdk_candidates if path.is_file()})
    identity = {"spec": spec, "dataset_sha256": file_digest(dataset), "mem_que": mem_que,
                "demo_sha256": file_digest(demo), "sdk": {p.name: file_digest(p) for p in sdk_files},
                "chip": "cpu", "device_id": 0, "no_default_control": True}
    return spec, dataset, mem_que, identity


def evaluate(pipeline, selection, tool, model_root, spec_path, conf_path, demo):
    if not selection["ok"]:
        raise ValueError("Configuration, build or assets are not verified")
    spec, dataset, mem_que, test_inputs = effect_inputs(spec_path, conf_path, demo)
    test_fingerprint = digest(test_inputs)
    if spec["biz_name"] != pipeline["biz_name"]:
        raise ValueError("Effect specification business mismatch")
    catalog = native(tool, "catalog")
    biz = next(item["demo_biz"] for item in catalog["bizs"] if item["biz_name"] == pipeline["biz_name"])
    model_root = Path(model_root).resolve()
    bundle_root = model_root.parent
    # Use a temporary configuration inside the existing asset bundle; no model
    # weights are copied and no path escapes the Operator deployment root.
    with tempfile.TemporaryDirectory(prefix=".selection-", dir=bundle_root) as directory:
        temporary = Path(directory)
        relative = temporary.relative_to(bundle_root)
        (temporary / "pipeline.json").write_text(json.dumps(pipeline))
        model_paths = {model["model_id"]: str(within(model_root, model["model_path"]).relative_to(bundle_root))
                       for model in pipeline.get("models", [])}
        generated_conf = {"data": {"pipe_path": str(relative / "pipeline.json"),
                                   "model_paths": model_paths, "mem_que": mem_que}}
        (temporary / "pipeline.conf").write_text(json.dumps(generated_conf))
        command = [str(Path(demo).resolve()), "--biz", biz, "--config", str(relative / "pipeline.conf"),
                   "--dataset", str(dataset), "--output-dir", str(temporary / "results"),
                   "--chip", "cpu", "--device-id", "0", "--batch-size", "1", "--depth", "1",
                   "--no-default-control"]
        process = subprocess.run(command, cwd=bundle_root, text=True, capture_output=True, timeout=1800, check=False)
        if process.returncode:
            raise ValueError("Effect run failed: " + (process.stdout + process.stderr)[-3000:])
        records = [json.loads(line) for line in (temporary / "results" / biz / "results.jsonl").read_text().splitlines() if line.strip()]
    if digest(effect_inputs(spec_path, conf_path, demo)[3]) != test_fingerprint:
        raise ValueError("Effect inputs or binaries changed during execution")
    metrics = compare_samples(records, spec)
    return {"schema_version": 1, "generated_at_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "selection_fingerprint": selection["selection_fingerprint"], "test_fingerprint": test_fingerprint,
            "pipeline": pipeline, "selection": selection, "test_inputs": test_inputs,
            "metrics": metrics, "records": records}


def attach_evidence(selection, evidence_path, spec_path, conf_path, demo):
    evidence = read_json(evidence_path)
    spec, _, _, identity = effect_inputs(spec_path, conf_path, demo)
    if (evidence.get("selection_fingerprint") != selection["selection_fingerprint"] or
            evidence.get("test_fingerprint") != digest(identity)):
        selection["effects"] = {"status": "stale", "reason": "Selection, dataset, criteria, deployment or executable changed"}
    else:
        selection["effects"] = compare_samples(evidence["records"], spec)
        selection["effects"]["generated_at_utc"] = evidence.get("generated_at_utc")
    selection["ready_for_business"] = selection["ok"] and selection["effects"]["status"] == "passed"
    return selection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["check", "evaluate"])
    parser.add_argument("--pipeline", required=True, type=Path)
    parser.add_argument("--tool", type=Path, default=ROOT / "build/alg_pipeline_tool")
    parser.add_argument("--demo", type=Path, default=ROOT / "build/alg_demo")
    parser.add_argument("--manifest", type=Path, default=MANIFEST)
    parser.add_argument("--model-root", type=Path, default=ROOT / "models")
    parser.add_argument("--variant")
    parser.add_argument("--effects", type=Path)
    parser.add_argument("--conf", type=Path)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-effects", action="store_true")
    args = parser.parse_args()
    try:
        pipeline = read_json(args.pipeline)
        report = inspect_selection(pipeline, args.tool, args.model_root, args.manifest, args.variant)
        conf = args.conf or args.pipeline.with_suffix(".conf")
        if args.command == "evaluate":
            if not args.effects or not args.output:
                parser.error("evaluate requires --effects and --output")
            report = evaluate(pipeline, report, args.tool, args.model_root, args.effects, conf, args.demo)
            # Re-hash assets after inference to detect mid-run changes.
            if inspect_selection(pipeline, args.tool, args.model_root, args.manifest, args.variant)["selection_fingerprint"] != report["selection_fingerprint"]:
                raise ValueError("Selection assets changed during execution")
            ok = report["metrics"]["status"] == "passed"
        else:
            if args.evidence:
                if not args.effects:
                    parser.error("--evidence requires --effects")
                attach_evidence(report, args.evidence, args.effects, conf, args.demo)
            ok = report["ready_for_business"] if args.require_effects else report["ok"]
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
