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


def dump_compact_json(data):
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def encode_request(payload):
    request = json.loads(payload)
    if not isinstance(request, dict):
        raise ValueError("Input must be a JSON object")
    # 仅为按行读取的数据集读取器规整空白。所有字段都会送达 SDK；
    # query 的选择与校验归其 Adapter 负责。
    return dump_compact_json(request)


def prepare_requests(payloads):
    texts = [encode_request(payload) for payload in payloads]
    if not texts:
        raise ValueError("Input dataset is empty")
    return texts


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
        # 原样转发完整的 SDK 响应，此处不做业务字段投影。
        responses[request_id] = dump_compact_json(document)
    return [responses[30001 + i] for i in range(count)]


def _run_demo_impl(requests, config, work_dir, executable):
    dataset = work_dir / "input.txt"
    dataset.write_text("\n".join(requests) + "\n", encoding="utf-8")
    output_dir = work_dir / "results"
    command = [
        str(executable), "--config", str(config),
        "--dataset", str(dataset), "--output-dir", str(output_dir),
    ]
    # 原生诊断输出不混入 JSON 字符串响应流。
    with (work_dir / "demo.log").open("w", encoding="utf-8") as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise ValueError(f"alg_demo failed (exit {result.returncode}); see {work_dir / 'demo.log'}")
    result_files = list(output_dir.glob("*/results.jsonl"))
    if len(result_files) != 1:
        raise ValueError("Demo must produce exactly one results.jsonl in the run output directory")
    return collect_responses(result_files[0], len(requests))


def run_demo(payloads, config, work_dir, executable):
    requests = prepare_requests(payloads)
    return _run_demo_impl(requests, config, work_dir, executable)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--input", help="One JSON request string; otherwise read stdin")
    source.add_argument("--dataset", type=Path, help="UTF-8 JSONL requests, one object per line")
    parser.add_argument("--config", default="configs/pipeline_translate_cpu.conf")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "results/translate",
                        help="Parent directory for a unique run's dataset, logs and results")
    parser.add_argument("--demo-bin", type=Path, default=ROOT / "build/alg_demo")
    args = parser.parse_args(argv)
    try:
        if args.dataset:
            payloads = args.dataset.read_text(encoding="utf-8").splitlines()
        else:
            payloads = [args.input if args.input is not None else sys.stdin.read()]
        # 在创建运行产物或启动模型前拒绝非法请求。
        texts = prepare_requests(payloads)
        args.output_dir.mkdir(parents=True, exist_ok=True)
        work_dir = Path(tempfile.mkdtemp(prefix="run-", dir=args.output_dir.resolve()))
        print(f"Demo artifacts: {work_dir}", file=sys.stderr)
        responses = _run_demo_impl(texts, args.config,
                                   work_dir, args.demo_bin.resolve())
        # 整次运行校验通过后才发布任何响应。
        sys.stdout.write("\n".join(responses) + "\n")
        return 0
    except (OSError, ValueError, TypeError, KeyError, AttributeError) as error:
        print(f"JSON prompt demo: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
