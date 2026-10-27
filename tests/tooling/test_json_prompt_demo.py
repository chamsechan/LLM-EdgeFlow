#!/usr/bin/env python3
"""JSON 启动器保留完整的请求文档和 SDK 响应文档。"""

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "demo/json_prompt_demo.py"
SPEC = importlib.util.spec_from_file_location("json_prompt_under_test", SCRIPT)
launcher = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(launcher)
DEMO = Path(os.environ.get("LLM_EDGEFLOW_DEMO_BINARY", ROOT / "build/alg_demo"))
CONFIG = ROOT / "demo/fixtures/mock/pipeline_translate.conf"


class JsonPromptConsumerTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="json-prompt-consumer-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def test_request_encoding_preserves_query_utf8_bytes_and_all_other_fields(self):
        query = '  Keep 中文\r\n"quotes" \\ and trailing space  '
        document = {"query": query, "extension": {"array": [False, 0, None]},
                    "second_text": "untouched", "request_id": 17}
        payload = json.dumps(document, ensure_ascii=False, indent=2)
        encoded = launcher.prepare_requests([payload])[0]
        self.assertNotIn('\n', encoded)
        forwarded = json.loads(encoded)
        self.assertEqual(forwarded, document)
        self.assertEqual(forwarded["query"].encode(), query.encode())
        # 字段选择和必填 query 校验留在 Adapter 中。
        self.assertEqual(json.loads(launcher.encode_request('{"other":"kept"}')), {"other": "kept"})

    def test_invalid_container_or_empty_dataset_is_rejected_before_creating_run_artifacts(self):
        for arguments in (("--input", "[]"), ("--input", "{broken")):
            with self.subTest(arguments=arguments):
                result = subprocess.run([sys.executable, str(SCRIPT), *arguments,
                                         "--output-dir", str(self.directory / 'absent')],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, '')
                self.assertIn('JSON prompt demo:', result.stderr)
                self.assertFalse((self.directory / 'absent').exists())
        with self.assertRaisesRegex(ValueError, 'empty'):
            launcher.prepare_requests([])

    def test_response_collection_preserves_complete_unknown_fields_and_nested_values(self):
        document = {"translation": "  响应\n第二行  ", "new_sdk_field": [False, 0, {"keep": None}]}
        result_file = self.directory / 'results.jsonl'
        result_file.write_text(json.dumps({"status": 0, "output": document}, ensure_ascii=False) + '\n')
        responses = launcher.collect_responses(result_file, 1)
        self.assertEqual(len(responses), 1)
        self.assertEqual(json.loads(responses[0]), document)
        self.assertNotIn('\n', responses[0])

    def test_response_count_status_and_document_shape_fail_as_one_run(self):
        result_file = self.directory / 'results.jsonl'
        good = {"status": 0, "output": {"translation": "first"}}
        for records, count, message in (([good], 2, 'number'),
                                         ([good, {"status": -7, "output": {}}], 2, 'failed sample'),
                                         ([{"status": 0, "output": "not a document"}], 1, 'object')):
            with self.subTest(message=message):
                result_file.write_text('\n'.join(json.dumps(record) for record in records) + '\n')
                with self.assertRaisesRegex(ValueError, message):
                    launcher.collect_responses(result_file, count)

    def test_real_demo_artifact_output_is_forwarded_without_python_projection(self):
        self.assertTrue(DEMO.is_file(), f"Set LLM_EDGEFLOW_DEMO_BINARY: {DEMO}")
        payloads = [json.dumps({"query": '  hello 中文\nworld  ', "extra": [False, 0]}),
                    json.dumps({"query": 'second request', "metadata": {"keep": 'yes'}})]
        work = self.directory / 'run'
        work.mkdir()
        responses = launcher.run_demo(payloads, CONFIG, work, DEMO.resolve())
        dataset = (work / 'input.txt').read_text().splitlines()
        self.assertEqual([json.loads(line) for line in dataset], [json.loads(line) for line in payloads])
        artifacts = list((work / 'results').glob('*/results.jsonl'))
        self.assertEqual(len(artifacts), 1)
        records = [json.loads(line) for line in artifacts[0].read_text().splitlines()]
        self.assertEqual([json.loads(response) for response in responses], [record['output'] for record in records])
        self.assertEqual([record['status'] for record in records], [0, 0])
        self.assertTrue((work / 'demo.log').is_file())

    def test_public_stdin_and_jsonl_modes_publish_only_response_lines(self):
        self.assertTrue(DEMO.is_file(), f"Set LLM_EDGEFLOW_DEMO_BINARY: {DEMO}")
        request = '{"query":"input","extension":{"keep":false}}'
        dataset = self.directory / 'requests.jsonl'
        dataset.write_text(request + '\n' + request + '\n')
        for source, stdin, count in (([], request, 1), (["--dataset", str(dataset)], None, 2)):
            with self.subTest(source=source):
                output = self.directory / f'output-{count}'
                result = subprocess.run([sys.executable, str(SCRIPT), *source,
                                         '--config', str(CONFIG), '--demo-bin', str(DEMO.resolve()),
                                         '--output-dir', str(output)], input=stdin,
                                        capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stderr)
                lines = result.stdout.splitlines()
                self.assertEqual(len(lines), count)
                self.assertTrue(all(isinstance(json.loads(line), dict) for line in lines))
                self.assertIn('Demo artifacts:', result.stderr)
                self.assertEqual(len(list(output.glob('run-*'))), 1)

    def test_native_failed_sample_never_publishes_a_partial_json_response_stream(self):
        self.assertTrue(DEMO.is_file(), f"Set LLM_EDGEFLOW_DEMO_BINARY: {DEMO}")
        dataset = self.directory / 'mixed.jsonl'
        dataset.write_text('{"query":"valid first"}\n{"wrong_field":"SDK must reject"}\n')
        result = subprocess.run([sys.executable, str(SCRIPT), '--dataset', str(dataset),
                                 '--config', str(CONFIG), '--demo-bin', str(DEMO.resolve()),
                                 '--output-dir', str(self.directory / 'failed')],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, '')
        self.assertIn('alg_demo failed', result.stderr)


if __name__ == '__main__':
    unittest.main()
