#!/usr/bin/env python3
"""LLM-EdgeFlow 的终端查看器和本地 Pipeline Studio 服务器。"""

from __future__ import annotations

import argparse
from enum import IntEnum
import hashlib
import http.server
import json
import os
from pathlib import Path
import re
import socketserver
import subprocess
import tempfile
import threading
from typing import Any
from urllib.parse import parse_qs, urlparse
import webbrowser


PROJECT_ROOT = Path(__file__).resolve().parents[2]
# 直接共用原生框架的名称和取值，Studio 不另建一份代码表。
ErrorCode = IntEnum("ErrorCode", {
    name: int(value)
    for value, name in re.findall(
        r'X\(k\w+,\s*(-?\d+),\s*"([A-Z][A-Z0-9_]*)"\)',
        (PROJECT_ROOT / "include/contracts/error_codes.h").read_text(encoding="utf-8"),
    )
})
WEB_ROOT = PROJECT_ROOT / "tools" / "pipeline_studio" / "web"
CONFIG_ROOT = PROJECT_ROOT / "configs"
PIPELINE_TOOL = Path(
    os.environ.get(
        "LLM_EDGEFLOW_PIPELINE_TOOL",
        PROJECT_ROOT / "build" / "alg_pipeline_tool",
    )
)
MAX_DOCUMENT_BYTES = 4 * 1024 * 1024


class StudioError(RuntimeError):
    def __init__(self, code: ErrorCode, message: str, status: int = 400):
        super().__init__(message)
        self.code = code.name
        self.status = status


class StudioHttpServer(http.server.ThreadingHTTPServer):
    """绑定时不做反向 DNS 查询的回环 HTTP 服务器。"""

    def server_bind(self) -> None:
        socketserver.TCPServer.server_bind(self)
        host, port = self.server_address[:2]
        self.server_name = host
        self.server_port = port


def json_result(ok: bool, **values: Any) -> dict[str, Any]:
    return {"ok": ok, **values}


def revision_for(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class WorkbenchService:
    """/api 背后的状态与文件系统边界。"""

    def __init__(self, config_root: Path = CONFIG_ROOT, initial: Path | None = None):
        self.config_root = config_root.resolve()
        self.save_lock = threading.Lock()
        self.initial_document = None
        if initial is not None:
            raw, pipeline = read_pipeline_document(initial)
            self.initial_document = json_result(
                True, filename=initial.name, revision="", pipeline=pipeline, imported=True
            )
            # 只有现有的托管文件契约才授予覆盖权限。
            try:
                managed = self.managed_path(initial.name, must_exist=True)
                if initial.resolve() == managed.resolve():
                    self.initial_document = json_result(
                        True, filename=initial.name, revision=revision_for(raw),
                        pipeline=pipeline, imported=False
                    )
            except StudioError:
                pass

    def initial_pipeline(self) -> dict[str, Any]:
        document = self.initial_document
        if document is not None and not document.get("imported", False):
            document = self.open_pipeline(document["filename"])
        return json_result(True, document=document)

    def managed_path(self, filename: str, must_exist: bool = False) -> Path:
        if not isinstance(filename, str) or Path(filename).name != filename:
            raise StudioError(ErrorCode.INVALID_PIPELINE_PATH, "请提供 configs 下的文件名，不含目录")
        if not re.fullmatch(r"pipeline_[a-z0-9_]+\.json", filename):
            raise StudioError(ErrorCode.INVALID_PIPELINE_NAME, "文件名必须匹配 pipeline_[a-z0-9_]+.json")
        path = self.config_root / filename
        if path.is_symlink():
            raise StudioError(ErrorCode.SYMLINK_REJECTED, "拒绝读写符号链接配置")
        if must_exist and not path.is_file():
            raise StudioError(ErrorCode.PIPELINE_NOT_FOUND, filename, 404)
        return path

    def pipelines(self) -> dict[str, Any]:
        items = []
        for path in sorted(self.config_root.glob("pipeline_*.json")):
            try:
                checked = self.managed_path(path.name, must_exist=True)
                raw, _ = read_pipeline_document(checked)
            except StudioError:
                continue
            items.append({"filename": checked.name, "revision": revision_for(raw)})
        return json_result(True, pipelines=items)

    def open_pipeline(self, requested: str) -> dict[str, Any]:
        path = self.managed_path(requested, must_exist=True)
        raw, pipeline = read_pipeline_document(path)
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
            raise StudioError(ErrorCode.TOOL_NOT_BUILT, "请先构建 build/alg_pipeline_tool", 503)
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
                ErrorCode.TOOL_PROTOCOL_ERROR,
                f"alg_pipeline_tool 未返回 JSON: {process.stderr[-500:]}",
                500,
            ) from error

    def catalog(self) -> dict[str, Any]:
        return self.invoke_tool(["catalog"])

    def validate(self, pipeline: Any) -> dict[str, Any]:
        return self.invoke_tool(["validate", "--stdin"], pipeline)

    def preview_authoring(self, request: dict[str, Any]) -> dict[str, Any]:
        return self.invoke_tool(["edit", "--stdin"], request)

    def init_pipeline(
        self, inputs: list[dict[str, Any]] | None = None,
        outputs: list[dict[str, Any]] | None = None,
    ) -> dict[str, Any]:
        args = ["init"]
        for option, entries in (("--input", inputs), ("--output", outputs)):
            if not isinstance(entries, list) or not entries:
                raise StudioError(ErrorCode.INVALID_INIT_REQUEST, "请选择输入和输出项")
            for entry in entries:
                if (not isinstance(entry, dict) or set(entry) != {"type", "name"}
                        or any(not isinstance(entry[field], str) or not entry[field]
                               for field in ("type", "name"))):
                    raise StudioError(ErrorCode.INVALID_INIT_REQUEST, "输入输出项必须指定 type 和 name")
                args.extend([option, entry["type"] + "/" + entry["name"]])
        return self.invoke_tool(args)

    def save_pipeline(
        self, requested: str, pipeline: Any, expected_revision: str | None,
        save_as: bool = False,
    ) -> dict[str, Any]:
        # 保存只是文档操作：未完成的图也可保存；执行前仍必须通过原生校验。
        if not is_pipeline_document(pipeline) or any(not isinstance(node, dict) for node in pipeline["pipeline"]):
            raise StudioError(ErrorCode.INVALID_PIPELINE_DOCUMENT, "方案必须包含节点对象数组 pipeline")
        object_array = lambda value: isinstance(value, list) and all(isinstance(item, dict) for item in value)
        if (any(node.get("depends_on") is not None and not isinstance(node["depends_on"], list) for node in pipeline["pipeline"])
                or pipeline.get("models") is not None and not object_array(pipeline["models"])
                or pipeline.get("io") is not None and (not isinstance(pipeline["io"], dict)
                    or any(pipeline["io"].get(direction) is not None and not object_array(pipeline["io"][direction])
                           for direction in ("input", "output")))):
            raise StudioError(ErrorCode.INVALID_PIPELINE_DOCUMENT, "节点、模型和输入输出项的文档结构不正确")
        try:
            encoded = (json.dumps(pipeline, ensure_ascii=False, indent=2, allow_nan=False) + "\n").encode()
        except (ValueError, TypeError) as error:
            raise StudioError(ErrorCode.INVALID_PIPELINE_DOCUMENT, "方案包含无效的 JSON 值") from error
        if len(encoded) > MAX_DOCUMENT_BYTES:
            raise StudioError(ErrorCode.DOCUMENT_TOO_LARGE, "方案文件超过 4 MiB")
        with self.save_lock:
            path = self.managed_path(requested)

            def check_revision() -> None:
                self.managed_path(requested)
                if path.exists():
                    if save_as:
                        raise StudioError(ErrorCode.FILE_EXISTS, "同名文件已存在，请换一个名称", 409)
                    if revision_for(path.read_bytes()) != expected_revision:
                        raise StudioError(ErrorCode.REVISION_CONFLICT, "文件已改变，请重新打开或另存", 409)
                elif expected_revision:
                    raise StudioError(ErrorCode.REVISION_CONFLICT, "文件已被删除，请另存", 409)

            check_revision()
            self.config_root.mkdir(parents=True, exist_ok=True)
            staged = None
            try:
                descriptor, name = tempfile.mkstemp(prefix=f".{path.stem}.", suffix=".json", dir=self.config_root)
                staged = Path(name)
                with os.fdopen(descriptor, "wb") as stream:
                    stream.write(encoded)
                    stream.flush()
                    os.fsync(stream.fileno())
                check_revision()
                if save_as:
                    # 以硬链接发布完整文件，不覆盖其他程序同时创建的同名目标。
                    os.link(staged, path)
                else:
                    os.replace(staged, path)
            except FileExistsError as error:
                raise StudioError(ErrorCode.FILE_EXISTS, "同名文件已存在，请换一个名称", 409) from error
            except OSError as error:
                raise StudioError(ErrorCode.SAVE_FAILED, str(error), 500) from error
            finally:
                if staged is not None:
                    staged.unlink(missing_ok=True)
            return json_result(True, filename=path.name, revision=revision_for(encoded), pipeline=pipeline)

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
                raise StudioError(ErrorCode.BODY_TOO_LARGE, "请求体超过 4 MiB", 413)
            try:
                return json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError as error:
                raise StudioError(ErrorCode.JSON_PARSE, str(error)) from error

        def _dispatch(self, method: str) -> None:
            parsed = urlparse(self.path)
            path = parsed.path
            query = parse_qs(parsed.query)
            body: dict[str, Any] = {}
            if method in ("POST", "PUT"):
                body = self._body()
            if method == "GET" and path == "/api/catalog":
                payload = service.catalog()
            elif method == "GET" and path == "/api/pipelines":
                payload = service.pipelines()
            elif method == "GET" and path == "/api/pipeline":
                payload = service.open_pipeline(query.get("filename", [""])[0])
            elif method == "GET" and path == "/api/initial":
                payload = service.initial_pipeline()
            elif method == "POST" and path == "/api/validate":
                payload = service.validate(body.get("pipeline"))
            elif method == "POST" and path == "/api/authoring/preview":
                payload = service.preview_authoring(body)
            elif method == "POST" and path == "/api/init":
                payload = service.init_pipeline(body.get("input"), body.get("output"))
            elif method == "POST" and path == "/api/pipelines":
                payload = service.save_pipeline(
                    body.get("filename", ""), body.get("pipeline"), None, save_as=True
                )
            elif method == "PUT" and path == "/api/pipeline":
                payload = service.save_pipeline(
                    body.get("filename", ""),
                    body.get("pipeline"),
                    body.get("revision"),
                    save_as=False,
                )
            else:
                raise StudioError(ErrorCode.NOT_FOUND, path, 404)
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

        def _respond(self, method: str) -> None:
            try:
                self._dispatch(method)
            except StudioError as error:
                self._send(
                    error.status,
                    json_result(False, error={"code": error.code, "message": str(error)}),
                )
            except Exception as error:
                self._send(
                    500,
                    json_result(
                        False,
                        error={"code": ErrorCode.INTERNAL_EXCEPTION.name, "message": str(error)},
                    ),
                )

    return Handler


def render_terminal(path: Path, pipeline: dict[str, Any]) -> None:
    print(f"\nLLM-EdgeFlow Pipeline: {path}")
    io = pipeline.get("io") or {}
    inputs, outputs = (io.get(side) or [] for side in ("input", "output"))
    pair = lambda item: f"{item.get('type', 'unknown')}/{item.get('name', '<missing-name>')}"
    pairs = lambda items: ", ".join(pair(item) for item in items)
    print(f"io: {pairs(inputs)} -> {pairs(outputs)}")
    for model in pipeline.get("models") or []:
        print(f"  model {model.get('name', '<missing-name>')}: {model.get('type', 'unknown')}"
              f" / {(model.get('backend') or {}).get('type', 'unknown')} / {model.get('file', '')}")
    for index, node in enumerate(pipeline.get("pipeline", [])):
        name = node.get("name", "<missing-name>")
        print(f"  [{index}] {name}: {node.get('type', 'unknown')}")
        mapping = json.dumps(node["inputs"], ensure_ascii=False) if "inputs" in node else "<未声明>"
        print(f"      inputs: {mapping}")
        print(f"      depends_on: {json.dumps(node.get('depends_on', []), ensure_ascii=False)} (额外顺序)")
    for output in outputs:
        print(f"  output {pair(output)}: "
              f"{json.dumps(output.get('inputs', {}), ensure_ascii=False)}")
    print()


def is_pipeline_document(value: Any) -> bool:
    return isinstance(value, dict) and isinstance(value.get("pipeline"), list)


def read_pipeline_document(path: Path) -> tuple[bytes, dict[str, Any]]:
    if path.suffix.lower() != ".json" or not path.is_file():
        raise StudioError(ErrorCode.INVALID_PIPELINE_FILE, "请选择一个 Pipeline JSON 文件")
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_DOCUMENT_BYTES + 1)
        if len(raw) > MAX_DOCUMENT_BYTES:
            raise StudioError(ErrorCode.DOCUMENT_TOO_LARGE, "方案文件超过 4 MiB")
        pipeline = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise StudioError(ErrorCode.JSON_PARSE, f"无法解析 JSON：{error}") from error
    except OSError as error:
        raise StudioError(ErrorCode.PIPELINE_READ_FAILED, f"无法读取方案文件：{error}") from error
    if not is_pipeline_document(pipeline):
        raise StudioError(ErrorCode.INVALID_PIPELINE_DOCUMENT, "方案必须是包含 pipeline 数组的 JSON 对象")
    return raw, pipeline


def read_pipeline_file(path: Path) -> dict[str, Any]:
    return read_pipeline_document(path)[1]


def launch_web(initial: Path | None, port: int) -> None:
    service = WorkbenchService(initial=initial)
    server = StudioHttpServer(("127.0.0.1", port), make_handler(service))
    actual_port = server.server_address[1]
    url = f"http://127.0.0.1:{actual_port}/index.html"
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


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(description="LLM-EdgeFlow Pipeline viewer/studio")
    parser.add_argument("pipeline", nargs="?", type=Path, help="要打开的 Pipeline JSON 文件")
    parser.add_argument("--web", action="store_true", help="启动 Web 工作台；默认在终端显示文件")
    parser.add_argument("--port", type=int, default=8080, help="Web 服务端口（默认 8080，仅 --web 使用）")
    args = parser.parse_args(argv)
    if args.web:
        launch_web(args.pipeline, args.port)
    elif args.pipeline is not None:
        render_terminal(args.pipeline, read_pipeline_file(args.pipeline))
    else:
        parser.print_help()


if __name__ == "__main__":
    try:
        main()
    except StudioError as error:
        raise SystemExit(f"[{error.code}] {error}") from error
