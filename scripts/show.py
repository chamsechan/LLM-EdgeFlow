#!/usr/bin/env python3
"""Terminal viewer and local Pipeline Studio server for LLM-EdgeFlow."""

from __future__ import annotations

import argparse
import copy
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import signal
import socketserver
import subprocess
import tempfile
import threading
import time
from typing import Any
from urllib.parse import parse_qs, quote, urlparse
import uuid
import webbrowser


PROJECT_ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = PROJECT_ROOT / "tools" / "visualizer"
CONFIG_ROOT = PROJECT_ROOT / "configs"
PROFILE_FILE = PROJECT_ROOT / "demo" / "profiles.json"
PIPELINE_TOOL = Path(
    os.environ.get(
        "LLM_EDGEFLOW_PIPELINE_TOOL",
        PROJECT_ROOT / "build" / "alg_pipeline_tool",
    )
)
DEMO_BINARY = Path(
    os.environ.get(
        "LLM_EDGEFLOW_DEMO_BINARY", PROJECT_ROOT / "build" / "alg_demo"
    )
)
MANAGED_NAME = re.compile(r"^pipeline_[a-z0-9_]+\.json$")
MAX_LOG_BYTES = 2 * 1024 * 1024


class StudioError(RuntimeError):
    def __init__(self, code: str, message: str, status: int = 400):
        super().__init__(message)
        self.code = code
        self.status = status


class StudioHttpServer(http.server.ThreadingHTTPServer):
    """Loopback HTTP server without reverse-DNS lookup during bind."""

    def server_bind(self) -> None:
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port


def json_result(ok: bool, **values: Any) -> dict[str, Any]:
    return {"schema_version": 1, "ok": ok, **values}


def revision_for(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


class WorkbenchService:
    """State and filesystem boundary behind /api/v1."""

    def __init__(self, config_root: Path = CONFIG_ROOT):
        self.config_root = config_root.resolve()
        self.jobs: dict[str, dict[str, Any]] = {}
        self.job_lock = threading.Lock()

    def managed_path(self, requested: str, must_exist: bool = False) -> Path:
        path = Path(requested)
        if path.is_absolute() or len(path.parts) not in (1, 2):
            raise StudioError("INVALID_PIPELINE_PATH", "只允许 configs 下的方案文件")
        if len(path.parts) == 2 and path.parts[0] != "configs":
            raise StudioError("INVALID_PIPELINE_PATH", "路径必须位于 configs 目录")
        filename = path.name
        if not MANAGED_NAME.fullmatch(filename):
            raise StudioError(
                "INVALID_PIPELINE_NAME",
                "文件名必须匹配 pipeline_[a-z0-9_]+.json",
            )
        candidate = self.config_root / filename
        if candidate.exists() or candidate.is_symlink():
            if candidate.is_symlink():
                raise StudioError("SYMLINK_REJECTED", "拒绝读写符号链接方案")
            if candidate.resolve().parent != self.config_root:
                raise StudioError("PATH_ESCAPE", "方案路径逃逸 configs 目录")
        elif must_exist:
            raise StudioError("PIPELINE_NOT_FOUND", filename, 404)
        return candidate

    def pipelines(self) -> dict[str, Any]:
        items = []
        for path in sorted(self.config_root.glob("pipeline_*.json")):
            try:
                checked = self.managed_path(path.name, must_exist=True)
                raw = checked.read_bytes()
                pipeline = json.loads(raw)
            except (StudioError, OSError, json.JSONDecodeError):
                continue
            items.append(
                {
                    "filename": checked.name,
                    "biz_name": pipeline.get("biz_name", ""),
                    "revision": revision_for(raw),
                }
            )
        return json_result(True, pipelines=items)

    def open_pipeline(self, requested: str) -> dict[str, Any]:
        path = self.managed_path(requested, must_exist=True)
        raw = path.read_bytes()
        try:
            pipeline = json.loads(raw)
        except json.JSONDecodeError as error:
            raise StudioError("INVALID_JSON", str(error)) from error
        return json_result(
            True,
            filename=path.name,
            revision=revision_for(raw),
            pipeline=pipeline,
        )

    def invoke_tool(
        self, command: list[str], pipeline: Any | None = None
    ) -> dict[str, Any]:
        if not PIPELINE_TOOL.is_file():
            raise StudioError("TOOL_NOT_BUILT", "请先构建 build/alg_pipeline_tool", 503)
        process = subprocess.run(
            [str(PIPELINE_TOOL), *command],
            input=None if pipeline is None else json.dumps(pipeline),
            text=True,
            capture_output=True,
            cwd=PROJECT_ROOT,
            timeout=30,
            check=False,
        )
        try:
            return json.loads(process.stdout)
        except json.JSONDecodeError as error:
            raise StudioError(
                "TOOL_PROTOCOL_ERROR",
                f"alg_pipeline_tool 未返回 JSON: {process.stderr[-500:]}",
                500,
            ) from error

    def catalog(self, biz: str = "") -> dict[str, Any]:
        args = ["catalog"]
        if biz:
            args.extend(["--biz", biz])
        return self.invoke_tool(args)

    def profiles(self) -> dict[str, Any]:
        root = read_json(PROFILE_FILE)
        profiles = []
        for name, profile in sorted(root.get("profiles", {}).items()):
            item = dict(profile)
            item["name"] = name
            profiles.append(item)
        return json_result(True, profiles=profiles)

    def validate(self, pipeline: Any) -> dict[str, Any]:
        return self.invoke_tool(["validate", "--stdin"], pipeline)

    def normalize(self, pipeline: Any) -> dict[str, Any]:
        return self.invoke_tool(["normalize", "--explicit-dag", "--stdin"], pipeline)

    def init_pipeline(
        self, biz: str, profile: str = "", empty: bool = False
    ) -> dict[str, Any]:
        args = ["init", "--biz", biz]
        if profile:
            args.extend(["--profile", profile])
        elif empty:
            args.append("--empty")
        return self.invoke_tool(args)

    def save_pipeline(
        self,
        requested: str,
        pipeline: Any,
        expected_revision: str | None,
        save_as: bool = False,
    ) -> dict[str, Any]:
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
        path = self.managed_path(requested)
        if path.exists() and not save_as:
            current = revision_for(path.read_bytes())
            if not expected_revision or current != expected_revision:
                raise StudioError(
                    "REVISION_CONFLICT",
                    "文件已被 IDE 或 Git 修改，请重新加载或另存",
                    409,
                )
        if save_as and path.exists():
            raise StudioError("FILE_EXISTS", "另存目标已存在", 409)
        encoded = (json.dumps(pipeline, ensure_ascii=False, indent=2) + "\n").encode()
        self.config_root.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(
            prefix=f".{path.stem}.", suffix=".tmp", dir=self.config_root
        )
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(encoded)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)
        return json_result(
            True,
            filename=path.name,
            revision=revision_for(encoded),
            pipeline=pipeline,
        )

    def start_run(self, pipeline: Any, profile_name: str) -> dict[str, Any]:
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
        profiles = read_json(PROFILE_FILE).get("profiles", {})
        profile = profiles.get(profile_name)
        if not profile:
            raise StudioError("UNKNOWN_PROFILE", profile_name)
        profile_conf = PROJECT_ROOT / profile["config"]
        conf = read_json(profile_conf)
        data = conf.get("data", conf)
        original_pipeline_path = Path(data["pipe_path"])
        if not original_pipeline_path.is_absolute():
            if (PROJECT_ROOT / original_pipeline_path).exists():
                original_pipeline_path = PROJECT_ROOT / original_pipeline_path
            else:
                original_pipeline_path = profile_conf.parent / original_pipeline_path
        original = read_json(original_pipeline_path.resolve())
        orig_biz = original.get("biz_name")
        curr_biz = pipeline.get("biz_name")
        if orig_biz != curr_biz:
            raise StudioError("PROFILE_MISMATCH", "Profile 与业务契约不匹配")
        with self.job_lock:
            if any(job["status"] in ("queued", "running") for job in self.jobs.values()):
                raise StudioError("RUN_BUSY", "同一工作台最多运行一个任务", 409)
            job_id = uuid.uuid4().hex[:16]
            self.jobs[job_id] = {
                "id": job_id,
                "status": "queued",
                "profile": profile_name,
                "logs": "",
                "cancel_requested": False,
                "process": None,
            }
        threading.Thread(
            target=self._run_job,
            args=(job_id, pipeline, copy.deepcopy(profile), conf),
            daemon=True,
        ).start()
        return json_result(True, job_id=job_id, status="queued")

    def _run_job(
        self,
        job_id: str,
        pipeline: Any,
        profile: dict[str, Any],
        conf: dict[str, Any],
    ) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="llm-edgeflow-studio-")
        temp_root = Path(temporary.name)
        try:
            pipeline_path = temp_root / "pipeline.json"
            pipeline_path.write_text(
                json.dumps(pipeline, ensure_ascii=False, indent=2), encoding="utf-8"
            )
            temp_conf = copy.deepcopy(conf)
            if "data" in temp_conf:
                temp_conf["data"]["pipe_path"] = "pipeline.json"
            else:
                temp_conf["pipe_path"] = "pipeline.json"
            conf_path = temp_root / "pipeline.conf"
            conf_path.write_text(json.dumps(temp_conf, indent=2), encoding="utf-8")
            output_dir = temp_root / "results"
            dataset = PROJECT_ROOT / profile["dataset"]
            args = [
                str(DEMO_BINARY),
                "--biz",
                str(profile["biz"]),
                "--config",
                str(conf_path),
                "--dataset",
                str(dataset),
                "--output-dir",
                str(output_dir),
                "--batch-size",
                str(profile.get("batch_size", 1)),
                "--device-id",
                str(profile.get("device_id", 0)),
                "--chip",
                str(profile.get("chip", "ax650")),
                "--depth",
                str(profile.get("depth", 1)),
            ]
            timeout = 300 if profile.get("suite", "smoke") == "smoke" else 1800
            with self.job_lock:
                job = self.jobs[job_id]
                if job["cancel_requested"]:
                    job["status"] = "cancelled"
                    return
                job["status"] = "running"
                job["started_at"] = time.time()
            if not DEMO_BINARY.is_file():
                raise StudioError("DEMO_NOT_BUILT", "请先构建 build/alg_demo", 503)
            process = subprocess.Popen(
                args,
                cwd=PROJECT_ROOT,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=False,
                start_new_session=True,
            )
            with self.job_lock:
                self.jobs[job_id]["process"] = process
            try:
                stdout, stderr = process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGTERM)
                stdout, stderr = process.communicate(timeout=5)
                raise StudioError("RUN_TIMEOUT", f"运行超过 {timeout} 秒", 408)
            combined = (stdout + b"\n" + stderr)[-MAX_LOG_BYTES:]
            result_files: dict[str, Any] = {}
            for name in ("summary.json", "results.jsonl"):
                matches = list(output_dir.rglob(name)) if output_dir.exists() else []
                if not matches:
                    continue
                text = matches[0].read_text(encoding="utf-8", errors="replace")
                if name.endswith(".json"):
                    try:
                        result_files[name] = json.loads(text)
                    except json.JSONDecodeError:
                        result_files[name] = text
                else:
                    result_files[name] = [
                        json.loads(line) for line in text.splitlines() if line.strip()
                    ]
            with self.job_lock:
                job = self.jobs[job_id]
                job["logs"] = combined.decode("utf-8", errors="replace")
                job["exit_code"] = process.returncode
                job["result"] = result_files
                job["status"] = "cancelled" if job["cancel_requested"] else (
                    "completed" if process.returncode == 0 else "failed"
                )
                job["finished_at"] = time.time()
                job["process"] = None
        except Exception as error:
            with self.job_lock:
                job = self.jobs[job_id]
                job["status"] = "cancelled" if job["cancel_requested"] else "failed"
                job["error"] = {
                    "code": getattr(error, "code", "RUN_FAILED"),
                    "message": str(error),
                }
                job["finished_at"] = time.time()
                job["process"] = None
        finally:
            temporary.cleanup()

    def run_status(self, job_id: str) -> dict[str, Any]:
        with self.job_lock:
            job = self.jobs.get(job_id)
            if not job:
                raise StudioError("RUN_NOT_FOUND", job_id, 404)
            public = {key: value for key, value in job.items() if key != "process"}
        return json_result(True, job=public)

    def cancel_run(self, job_id: str) -> dict[str, Any]:
        with self.job_lock:
            job = self.jobs.get(job_id)
            if not job:
                raise StudioError("RUN_NOT_FOUND", job_id, 404)
            job["cancel_requested"] = True
            process = job.get("process")
            if job["status"] == "queued":
                job["status"] = "cancelled"
        if process and process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
        return json_result(True, job_id=job_id, status="cancelling")


def make_handler(service: WorkbenchService):
    class Handler(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *args: Any, **kwargs: Any):
            super().__init__(*args, directory=str(WEB_ROOT), **kwargs)

        def log_message(self, _format: str, *_args: Any) -> None:
            return

        def _send(self, status: int, payload: Any) -> None:
            encoded = json.dumps(payload, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(encoded)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(encoded)

        def _body(self) -> dict[str, Any]:
            length = int(self.headers.get("Content-Length", "0"))
            if length > 4 * 1024 * 1024:
                raise StudioError("BODY_TOO_LARGE", "请求体超过 4 MiB", 413)
            try:
                return json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as error:
                raise StudioError("INVALID_JSON", str(error)) from error

        def _dispatch(self, method: str) -> None:
            parsed = urlparse(self.path)
            path = parsed.path
            query = parse_qs(parsed.query)
            body: dict[str, Any] = {}
            if method in ("POST", "PUT", "DELETE"):
                body = self._body()
            if method == "GET" and path == "/api/v1/catalog":
                payload = service.catalog(query.get("biz", [""])[0])
            elif method == "GET" and path == "/api/v1/profiles":
                payload = service.profiles()
            elif method == "GET" and path == "/api/v1/pipelines":
                payload = service.pipelines()
            elif method == "GET" and path == "/api/v1/pipeline":
                payload = service.open_pipeline(query.get("filename", [""])[0])
            elif method == "GET" and path.startswith("/api/v1/runs/"):
                payload = service.run_status(path.rsplit("/", 1)[-1])
            elif method == "POST" and path == "/api/v1/validate":
                payload = service.validate(body.get("pipeline"))
            elif method == "POST" and path == "/api/v1/normalize":
                payload = service.normalize(body.get("pipeline"))
            elif method == "POST" and path == "/api/v1/init":
                payload = service.init_pipeline(
                    body.get("biz", ""), body.get("profile", ""), body.get("empty", False)
                )
            elif method == "POST" and path == "/api/v1/pipelines":
                payload = service.save_pipeline(
                    body.get("filename", ""), body.get("pipeline"), None, save_as=True
                )
            elif method == "PUT" and path == "/api/v1/pipeline":
                payload = service.save_pipeline(
                    body.get("filename", ""),
                    body.get("pipeline"),
                    body.get("revision"),
                    save_as=False,
                )
            elif method == "POST" and path == "/api/v1/runs":
                payload = service.start_run(body.get("pipeline"), body.get("profile", ""))
            elif method == "DELETE" and path.startswith("/api/v1/runs/"):
                payload = service.cancel_run(path.rsplit("/", 1)[-1])
            else:
                raise StudioError("NOT_FOUND", path, 404)
            self._send(200, payload)

        def do_GET(self) -> None:
            if not urlparse(self.path).path.startswith("/api/"):
                super().do_GET()
                return
            self._respond("GET")

        def do_POST(self) -> None:
            self._respond("POST")

        def do_PUT(self) -> None:
            self._respond("PUT")

        def do_DELETE(self) -> None:
            self._respond("DELETE")

        def _respond(self, method: str) -> None:
            try:
                self._dispatch(method)
            except StudioError as error:
                detail: Any = str(error)
                if error.code == "VALIDATION_FAILED":
                    try:
                        detail = json.loads(str(error))
                    except json.JSONDecodeError:
                        pass
                self._send(
                    error.status,
                    json_result(False, error={"code": error.code, "message": detail}),
                )
            except Exception as error:
                self._send(
                    500,
                    json_result(
                        False,
                        error={"code": "INTERNAL_ERROR", "message": str(error)},
                    ),
                )

    return Handler


def render_terminal(path: Path, pipeline: dict[str, Any]) -> None:
    print(f"\nLLM-EdgeFlow Pipeline: {path}")
    biz = pipeline.get("biz_name", "unknown")
    print(f"Biz: {biz}")
    nodes = pipeline.get("pipeline", [])
    for index, node in enumerate(nodes):
        node_id = node.get("id", f"node_{index}_{node.get('node_type', 'unknown')}")
        depends = node.get("depends_on")
        if depends is None and index:
            previous = nodes[index - 1]
            depends = [
                previous.get(
                    "id", f"node_{index - 1}_{previous.get('node_type', 'unknown')}"
                )
            ]
        print(f"  [{index}] {node_id}: {node.get('node_type', 'unknown')} <- {depends or []}")
    print()


def launch_web(initial: Path | None, port: int) -> None:
    service = WorkbenchService()
    server = StudioHttpServer(("127.0.0.1", port), make_handler(service))
    actual_port = server.server_address[1]
    fragment = f"#pipeline={quote(initial.name)}" if initial else ""
    url = f"http://127.0.0.1:{actual_port}/index.html{fragment}"
    print("LLM-EdgeFlow Pipeline Studio 已启动")
    print(f"地址: {url}")
    print("服务仅绑定 127.0.0.1；Ctrl+C 停止。")
    try:
        webbrowser.open(url)
    except Exception:
        pass
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


def main() -> None:
    parser = argparse.ArgumentParser(description="LLM-EdgeFlow Pipeline viewer/studio")
    parser.add_argument("pipeline", nargs="?", help="configs/pipeline_*.json")
    parser.add_argument("--web", "--ui", action="store_true", dest="web")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    selected: Path | None = None
    if args.pipeline:
        service = WorkbenchService()
        selected = service.managed_path(args.pipeline, must_exist=True)
        pipeline = read_json(selected)
        if not args.web:
            render_terminal(selected, pipeline)
    if args.web:
        launch_web(selected, args.port)
    elif not args.pipeline:
        parser.print_help()


if __name__ == "__main__":
    try:
        main()
    except StudioError as error:
        raise SystemExit(f"[{error.code}] {error}") from error
