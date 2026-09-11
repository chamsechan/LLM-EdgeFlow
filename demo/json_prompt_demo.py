#!/usr/bin/env python3
"""Map JSON strings onto the existing text/structured-JSON Demo contract.

Defaults implement translation. The SDK Adapter owns request field selection
and response formatting; this launcher forwards complete JSON objects.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def encode_request(payload):
    request = json.loads(payload)
    if not isinstance(request, dict):
        raise ValueError("Input must be a JSON object")
    # Only normalize whitespace for the line-based dataset reader. All fields
    # reach the SDK; query selection and validation belong to its Adapter.
    return json.dumps(request, ensure_ascii=False, separators=(",", ":"))


def collect_responses(result_file, count):
    records = [json.loads(line) for line in result_file.read_text(encoding="utf-8").splitlines()]
    if len(records) != count:
        raise ValueError("Demo returned an unexpected number of results")
    responses = {}
    for record in records:
        request_id = record.get("request_id")
        if type(request_id) is not int or not 30001 <= request_id < 30001 + count:
            raise ValueError("Demo returned an unexpected request ID")
        if request_id in responses or record.get("status") != 0:
            raise ValueError("Demo returned a duplicate request ID or a failed sample")
        document = record.get("output", {}).get("entities")
        if not isinstance(document, dict):
            raise ValueError("Demo result must contain a JSON response object")
        # Forward the complete SDK response. No business field projection here.
        responses[request_id] = json.dumps(
            document, ensure_ascii=False, separators=(",", ":")
        )
    return [responses[30001 + i] for i in range(count)]


def run_demo(payloads, config, biz, work_dir, executable):
    texts = [encode_request(payload) for payload in payloads]
    if not texts:
        raise ValueError("Input dataset is empty")
    dataset = work_dir / "input.txt"
    dataset.write_text("\n".join(texts) + "\n", encoding="utf-8")
    output_dir = work_dir / "results"
    command = [
        str(executable), "--biz", biz, "--config", str(config),
        "--dataset", str(dataset), "--output-dir", str(output_dir),
        "--batch-size", "1", "--chip", "cpu_generic",
    ]
    # Keep native diagnostic output out of the JSON string response stream.
    with (work_dir / "demo.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f"alg_demo failed (exit {result.returncode}); see {work_dir / 'demo.log'}")
    return collect_responses(output_dir / biz / "results.jsonl", len(texts))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--input", help="One JSON request string; otherwise read stdin")
    source.add_argument("--dataset", type=Path, help="UTF-8 JSONL requests, one object per line")
    parser.add_argument("--config", default="configs/pipeline_translate_cpu.conf")
    parser.add_argument("--biz", default="translate", help="Registered SDK/Demo contract")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "results/translate",
                        help="Parent directory for a unique run's dataset, logs and results")
    parser.add_argument("--demo-bin", type=Path, default=ROOT / "build/alg_demo")
    args = parser.parse_args(argv)
    try:
        if args.dataset:
            payloads = args.dataset.read_text(encoding="utf-8").splitlines()
        else:
            payloads = [args.input if args.input is not None else sys.stdin.read()]
        # Reject invalid requests before creating run artifacts or starting a model.
        for payload in payloads:
            encode_request(payload)
        if not payloads:
            raise ValueError("Input dataset is empty")
        args.output_dir.mkdir(parents=True, exist_ok=True)
        work_dir = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_dir.resolve()))
        print(f"Demo artifacts: {work_dir}", file=sys.stderr)
        responses = run_demo(payloads, args.config, args.biz,
                             work_dir, args.demo_bin.resolve())
        # Validate the entire run before publishing any response.
        sys.stdout.write("\n".join(responses) + "\n")
        return 0
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        print(f"JSON prompt demo: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
