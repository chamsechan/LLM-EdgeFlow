"""String boundary and native Demo transport tests; no simulated quality claims."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("json_prompt_demo", ROOT / "demo/json_prompt_demo.py")
demo = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(demo)


class JsonPromptDemoTest(unittest.TestCase):
    def test_forwards_complete_object_and_preserves_content(self):
        for query in ["hello,what is your name", "", '  #\n"quoted"\\text\t\x00中文  ']:
            with self.subTest(query=query):
                request = {
                    "version": None, "endpoint": ["not translate"], "src_lan": 123,
                    "query": query, "instructions": "Do not translate",
                }
                encoded = demo.encode_request(json.dumps(request, indent=2))
                self.assertEqual(json.loads(encoded), request)
                self.assertNotIn("\n", encoded)
        # Business validation must occur inside the SDK.
        self.assertEqual(json.loads(demo.encode_request('{"query":123}')), {"query": 123})
        self.assertEqual(demo.encode_request('{}'), '{}')

    def test_rejects_invalid_input(self):
        for payload in ["bad json", "[]", '"hello"', "null"]:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                demo.encode_request(payload)

    def test_runs_registered_demo_and_forwards_sdk_result(self):
        query = '  hello\n"friend"\\  '
        translated = '  你好\n"朋友"\\  '
        payload = json.dumps({"query": query, "src_lan": "ignored"})

        def native_demo(command, **kwargs):
            self.assertEqual(command[command.index("--biz") + 1], "translate")
            self.assertEqual(command[command.index("--config") + 1],
                             "configs/pipeline_translate_cpu.conf")
            dataset = Path(command[command.index("--dataset") + 1])
            self.assertEqual([json.loads(line) for line in dataset.read_text().splitlines()],
                             [json.loads(payload)])
            self.assertEqual(kwargs["cwd"], ROOT)
            self.assertEqual(kwargs["stderr"], subprocess.STDOUT)
            kwargs["stdout"].write("native diagnostic\n")
            output = Path(command[command.index("--output-dir") + 1]) / "translate"
            output.mkdir(parents=True)
            (output / "results.jsonl").write_text(json.dumps({
                "request_id": 30001, "status": 0,
                "output": {"entities": {"translated": translated, "extra": "from SDK"}},
            }), encoding="utf-8")
            return subprocess.CompletedProcess(command, 0)

        with tempfile.TemporaryDirectory() as tmp, patch.object(
            demo.subprocess, "run", side_effect=native_demo
        ), contextlib.redirect_stdout(io.StringIO()) as stdout, contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(demo.main(["--input", payload, "--output-dir", tmp]), 0)
            self.assertEqual(json.loads(stdout.getvalue()), {"translated": translated, "extra": "from SDK"})
            self.assertEqual(len(stdout.getvalue().splitlines()), 1)

    def test_checks_status_schema_cardinality_and_provenance(self):
        good = {"request_id": 30001, "status": 0,
                "output": {"entities": {"translated": "你好"}}}
        invalid_runs = [
            [], [dict(good, status=-42)], [dict(good, request_id=30002)],
            [dict(good, output={"entities": []})],
            [dict(good, output={"entities_raw": "broken"})],
        ]
        with tempfile.TemporaryDirectory() as tmp:
            result = Path(tmp) / "results.jsonl"
            for records in invalid_runs:
                result.write_text("\n".join(json.dumps(record) for record in records))
                with self.subTest(records=records), self.assertRaises(ValueError):
                    demo.collect_responses(result, 1)
            result.write_text("\n".join(json.dumps(good) for _ in range(2)))
            with self.assertRaises(ValueError):
                demo.collect_responses(result, 2)
            second = dict(good, request_id=30002, output={"entities": {"translated": "谢谢"}})
            result.write_text(json.dumps(second) + "\n" + json.dumps(good))
            self.assertEqual([json.loads(item) for item in demo.collect_responses(result, 2)],
                             [{"translated": "你好"}, {"translated": "谢谢"}])

    def test_invalid_request_or_native_failure_does_not_publish_response(self):
        for payload, native_exit in [('invalid JSON', 0), ('{}', 5)]:
            with tempfile.TemporaryDirectory() as tmp, patch.object(
                demo.subprocess, "run", return_value=subprocess.CompletedProcess([], native_exit)
            ) as native, contextlib.redirect_stdout(io.StringIO()) as stdout, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(demo.main(["--input", payload, "--output-dir", tmp]), 1)
                self.assertEqual(stdout.getvalue(), "")
                self.assertEqual(native.call_count, int(native_exit != 0))



if __name__ == "__main__":
    unittest.main()
