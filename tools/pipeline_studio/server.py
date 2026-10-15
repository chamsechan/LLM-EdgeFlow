#!/usr/bin/env python3
"""LLM-EdgeFlow 的终端查看器和本地 Pipeline Studio 服务器。"""

from __future__ import annotations

import argparse
import copy
from contextlib import contextmanager
from enum import IntEnum
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import signal
import socketserver
import subprocess
import tempfile
import threading
import time
from typing import Any
from urllib.parse import parse_qs, urlparse
import uuid
import webbrowser


PROJECT_ROOT = Path(__file__).resolve().parents[2]
# Share the native framework's names and values without a second Studio code table.
ErrorCode = IntEnum("ErrorCode", {
    name: int(value)
    for value, name in re.findall(
        r'X\(k\w+,\s*(-?\d+),\s*"([A-Z][A-Z0-9_]*)"\)',
        (PROJECT_ROOT / "include/contracts/error_codes.h").read_text(encoding="utf-8"),
    )
})
WEB_ROOT = PROJECT_ROOT / "tools" / "pipeline_studio" / "web"
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
_SELECTION_SPEC = importlib.util.spec_from_file_location("edgeflow_selection", PROJECT_ROOT / "tools/verify_selection.py")
SELECTION = importlib.util.module_from_spec(_SELECTION_SPEC)
_SELECTION_SPEC.loader.exec_module(SELECTION)
_PROFILE_SPEC = importlib.util.spec_from_file_location("edgeflow_demo_profile", PROJECT_ROOT / "demo/common/demo_profile.py")
DEMO_PROFILE = importlib.util.module_from_spec(_PROFILE_SPEC)
_PROFILE_SPEC.loader.exec_module(DEMO_PROFILE)

MAX_LOG_BYTES = 2 * 1024 * 1024
MAX_DOCUMENT_BYTES = 4 * 1024 * 1024
SMOKE_RUN_TIMEOUT_SECONDS = 300
REAL_RUN_TIMEOUT_SECONDS = 1800
PROCESS_STOP_TIMEOUT_SECONDS = 5


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


def apply_json_patch(doc: Any, patch: list[dict[str, Any]]) -> Any:
    """Apply the test/add/remove/replace/move operations emitted by native fixes."""
    doc = copy.deepcopy(doc)

    def parent(pointer: str) -> tuple[Any, str | int]:
        if not pointer.startswith("/"):
            raise ValueError(f"Invalid JSON Pointer: {pointer}")
        parts = [part.replace("~1", "/").replace("~0", "~") for part in pointer[1:].split("/")]
        target = doc
        for part in parts[:-1]:
            target = target[int(part)] if isinstance(target, list) else target[part]
        key = parts[-1]
        return target, int(key) if isinstance(target, list) and key != "-" else key

    for step in patch:
        op = step["op"]
        if op not in ("test", "add", "remove", "replace", "move"):
            raise ValueError(f"Unsupported fix operation: {op}")
        target, key = parent(step["path"])
        if op == "test":
            if target[key] != step["value"]:
                raise ValueError(f"Patch test failed at {step['path']}")
        elif op == "remove":
            del target[key]
        else:
            if op == "move":
                source, source_key = parent(step["from"])
                value = source.pop(source_key)
                target, key = parent(step["path"])
            else:
                value = copy.deepcopy(step["value"])
            if isinstance(target, list) and op in ("add", "move"):
                target.insert(len(target) if key == "-" else key, value)
            else:
                if op == "replace":
                    target[key]  # Replacing an absent field is not a valid fix.
                target[key] = value
    return doc


def read_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def get_tool_fingerprint() -> str:
    if PIPELINE_TOOL.is_file():
        digest = hashlib.sha256(str(PIPELINE_TOOL.resolve()).encode())
        with PIPELINE_TOOL.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return digest.hexdigest()
    return hashlib.sha256(str(PIPELINE_TOOL).encode()).hexdigest()[:16]


class WorkbenchService:
    """/api 背后的状态与文件系统边界。"""

    def __init__(self, config_root: Path = CONFIG_ROOT, initial: Path | None = None):
        self.config_root = config_root.resolve()
        self.jobs: dict[str, dict[str, Any]] = {}
        self.job_lock = threading.Lock()
        self.solution_lock = threading.Lock()
        # 所有权仅限于本服务器会话创建的配对。
        self.generated_solutions: dict[str, dict[str, Any]] = {}
        self.initial_document = None
        if initial is not None:
            pipeline = read_pipeline_file(initial)
            self.initial_document = json_result(
                True, filename=initial.name, revision="", pipeline=pipeline, imported=True
            )
            # 只有现有的托管文件契约才授予覆盖权限。
            try:
                managed = self.managed_path(initial.name, must_exist=True)
                if initial.resolve() == managed.resolve():
                    self.initial_document = self.open_pipeline(initial.name)
            except StudioError:
                pass

    def initial_pipeline(self) -> dict[str, Any]:
        document = self.initial_document
        if document is not None and not document.get("imported", False):
            document = self.open_pipeline(document["filename"])
        return json_result(True, document=document)

    def managed_path(self, filename: str, must_exist: bool = False, suffix: str = ".json") -> Path:
        kind = "PIPELINE" if suffix == ".json" else "DEPLOYMENT"
        if Path(filename).name != filename:
            raise StudioError(ErrorCode[f"INVALID_{kind}_PATH"], "请提供 configs 下的文件名，不含目录")
        if not re.fullmatch(r"pipeline_[a-z0-9_]+" + re.escape(suffix), filename):
            raise StudioError(ErrorCode[f"INVALID_{kind}_NAME"], f"文件名必须匹配 pipeline_[a-z0-9_]+{suffix}")
        path = self.config_root / filename
        if path.is_symlink():
            raise StudioError(ErrorCode.SYMLINK_REJECTED, "拒绝读写符号链接配置")
        if must_exist and not path.is_file():
            raise StudioError(ErrorCode[f"{kind}_NOT_FOUND"], filename, 404)
        return path

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
                    "io": self.io_pairs(pipeline),
                    "revision": revision_for(raw),
                }
            )
        confs = [p.name for p in sorted(self.config_root.glob("*.conf")) if p.is_file()]
        return json_result(True, pipelines=items, deployments=confs)

    @staticmethod
    def io_pairs(pipeline: Any) -> dict[str, list[dict[str, str]]]:
        inputs, outputs = SELECTION.pipeline_io(pipeline)
        def pairs(entries):
            if not isinstance(entries, list):
                return []
            return [{"type": item["type"], "name": item["name"]}
                    for item in entries if isinstance(item, dict)
                    and isinstance(item.get("type"), str)
                    and isinstance(item.get("name"), str)]
        return {"input": pairs(inputs), "output": pairs(outputs)}

    @contextmanager
    def temporary_documents(self, pipeline: Any, prefix: str):
        """快照保留原目录的资源基准，只清理本次创建的隐藏文件。"""
        self.config_root.mkdir(parents=True, exist_ok=True)
        paths: list[Path] = []
        try:
            descriptor, name = tempfile.mkstemp(prefix=prefix, suffix=".json", dir=self.config_root)
            pipeline_path = Path(name)
            paths.append(pipeline_path)
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(pipeline, stream, ensure_ascii=False, indent=2)
                stream.write("\n")
            descriptor, name = tempfile.mkstemp(prefix=prefix, suffix=".conf", dir=self.config_root)
            conf_path = Path(name)
            paths.append(conf_path)
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump({"pipe_path": pipeline_path.name}, stream)
                stream.write("\n")
            yield pipeline_path, conf_path
        finally:
            for path in paths:
                path.unlink(missing_ok=True)

    def open_pipeline(self, requested: str) -> dict[str, Any]:
        path = self.managed_path(requested, must_exist=True)
        raw = path.read_bytes()
        try:
            pipeline = json.loads(raw)
        except json.JSONDecodeError as error:
            raise StudioError(ErrorCode.JSON_PARSE, str(error)) from error
        return json_result(
            True,
            filename=path.name,
            revision=revision_for(raw),
            pipeline=pipeline,
            deployment=self.deployment_info(path.name),
        )

    def deployment_info(self, filename: str) -> dict[str, Any] | None:
        managed = self.generated_solutions.get(filename)
        if not managed:
            return None
        return {"conf_name": managed["conf_path"].name,
                "conf_revision": managed["conf_revision"]}

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

    def assets(self) -> dict[str, Any]:
        return {"ok": True, **SELECTION.asset_catalog(pipeline_dir=self.config_root)}

    def verify_selection(self, pipeline: Any, variant: str = "", filename: str = "") -> dict[str, Any]:
        try:
            pipeline_dir = None
            if filename:
                path = self.managed_path(filename, must_exist=True)
                pipeline_dir = path.parent
            return SELECTION.inspect_selection(
                pipeline, PIPELINE_TOOL, pipeline_dir, variant=variant or None)
        except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
            raise StudioError(ErrorCode.SELECTION_CHECK_FAILED, str(error)) from error

    def validate(self, pipeline: Any, explain: bool = False) -> dict[str, Any]:
        args = ["validate", "--stdin"]
        if explain:
            args.append("--explain")
        fingerprint = get_tool_fingerprint() if explain else None
        report = self.invoke_tool(args, pipeline)
        if explain and isinstance(report, dict):
            if fingerprint != get_tool_fingerprint():
                raise StudioError(ErrorCode.TOOL_OUTDATED, "校验期间工具已更新，请重新校验", 409)
            report["tool_fingerprint"] = fingerprint
            raw = json.dumps(pipeline, sort_keys=True).encode("utf-8")
            report["revision"] = revision_for(raw)
        return report

    def preview_fix(
        self,
        pipeline: Any,
        patch: list[dict[str, Any]],
        expected_revision: str | None = None,
        tool_fingerprint: str | None = None,
    ) -> dict[str, Any]:
        if not expected_revision:
            raise StudioError(ErrorCode.REVISION_CONFLICT, "必须指定预期修订版本，草稿可能已变更", 409)
        raw = json.dumps(pipeline, sort_keys=True).encode("utf-8")
        current_rev = revision_for(raw)
        if current_rev != expected_revision:
            raise StudioError(ErrorCode.REVISION_CONFLICT, "草稿已变更，候选失效", 409)
        current_fp = get_tool_fingerprint()
        if not tool_fingerprint or current_fp != tool_fingerprint:
            raise StudioError(ErrorCode.TOOL_OUTDATED, "底层校验工具已重建或更新，候选失效", 409)
        try:
            patched = apply_json_patch(pipeline, patch)
        except Exception as error:
            raise StudioError(
                ErrorCode.PATCH_APPLICATION_FAILED, f"补丁应用失败: {error}"
            ) from error
        report = self.validate(patched, explain=True)
        if current_fp != get_tool_fingerprint():
            raise StudioError(ErrorCode.TOOL_OUTDATED, "修复预览期间工具已更新，请重新校验", 409)
        return json_result(
            True,
            patched=patched,
            report=report,
            revision=revision_for(
                json.dumps(patched, sort_keys=True).encode("utf-8")
            ),
            tool_fingerprint=current_fp,
        )

    def preview_authoring(self, request: dict[str, Any]) -> dict[str, Any]:
        fingerprint = get_tool_fingerprint()
        result = self.invoke_tool(["edit", "--stdin"], request)
        if get_tool_fingerprint() != fingerprint:
            raise StudioError(ErrorCode.TOOL_OUTDATED, "操作期间工具已更新，请重新加载", 409)
        if result.get("pipeline") is not None:
            result["validation"]["tool_fingerprint"] = fingerprint
            result["validation"]["revision"] = revision_for(
                json.dumps(result["pipeline"], sort_keys=True).encode("utf-8"))
        return result

    def init_pipeline(
        self, inputs: list[dict[str, Any]] | None = None,
        outputs: list[dict[str, Any]] | None = None, profile: str = "",
    ) -> dict[str, Any]:
        args = ["init"]
        if profile:
            if inputs is not None or outputs is not None:
                raise StudioError(ErrorCode.INVALID_INIT_REQUEST, "Profile 不能与输入输出项同时指定")
            args.extend(["--profile", profile])
        else:
            for option, entries in (("--input", inputs), ("--output", outputs)):
                entries = [] if entries is None else entries
                if not isinstance(entries, list):
                    raise StudioError(ErrorCode.INVALID_INIT_REQUEST, "输入和输出项必须是列表")
                for entry in entries:
                    if (not isinstance(entry, dict) or set(entry) != {"type", "name"}
                            or any(not isinstance(entry[field], str) or not entry[field]
                                   for field in ("type", "name"))):
                        raise StudioError(ErrorCode.INVALID_INIT_REQUEST, "输入输出项必须指定 type 和 name")
                    args.extend([option, entry["type"] + "/" + entry["name"]])
        return self.invoke_tool(args)

    def save_pipeline(
        self, requested: str, pipeline: Any, expected_revision: str | None,
        save_as: bool = False, profile_name: str = "",
    ) -> dict[str, Any]:
        with self.solution_lock:
            path = self.managed_path(requested)
            managed = self.generated_solutions.get(path.name) if not save_as else None

            def check_revisions() -> dict[str, Any] | None:
                self.managed_path(path.name)
                if path.exists():
                    if save_as:
                        raise StudioError(ErrorCode.FILE_EXISTS, "另存目标已存在", 409)
                    if revision_for(path.read_bytes()) != expected_revision:
                        raise StudioError(ErrorCode.REVISION_CONFLICT, "文件已改变，请重新加载或另存", 409)
                elif expected_revision:
                    raise StudioError(ErrorCode.REVISION_CONFLICT, "文件已被删除，请重新加载或另存", 409)
                if managed:
                    return self.deployment_profile(pipeline, filename=path.name)
                return None

            profile = check_revisions()
            if managed and profile_name:
                profile = self.profile_inputs(pipeline, profile_name)
            try:
                with self.temporary_documents(pipeline, f".{path.stem}.") as (staged_json, staged_conf):
                    report = self.invoke_tool(["validate", str(staged_json)])
                    if not report.get("ok"):
                        raise StudioError(ErrorCode.VALIDATION_FAILED, json.dumps(report, ensure_ascii=False))
                    configuration = self.resolve_run_conf(staged_conf, profile) if managed else None
                    encoded = staged_json.read_bytes()
                    with staged_json.open("rb") as stream:
                        os.fsync(stream.fileno())
                    check_revisions()
                    os.replace(staged_json, path)
            except OSError as error:
                raise StudioError(ErrorCode.SAVE_FAILED, str(error), 500) from error
            if managed:
                managed["pipeline_revision"] = revision_for(encoded)
                managed["profile"] = profile
                configuration["conf_path"] = str(managed["conf_path"])
                configuration["pipeline_path"] = str(path)
                return self.solution_result(path, pipeline, encoded, profile, configuration)
            return json_result(True, filename=path.name, revision=revision_for(encoded),
                               pipeline=pipeline, deployment=None)

    def profile_inputs(self, pipeline: Any, profile_name: str) -> dict[str, Any]:
        profiles = read_json(PROFILE_FILE).get("profiles", {})
        if profile_name not in profiles:
            raise StudioError(ErrorCode.UNKNOWN_PROFILE, profile_name)
        profile = profiles[profile_name]
        try:
            DEMO_PROFILE.validate_profile_fields(profile)
        except ValueError as error:
            raise StudioError(ErrorCode.INVALID_PROFILE, f"Profile '{profile_name}': {error}") from error
        profile_conf = PROJECT_ROOT / profile["config"]
        configuration = self.resolve_run_conf(profile_conf, profile)
        original = read_json(Path(configuration["pipeline_path"]))
        if self.io_pairs(original) != self.io_pairs(pipeline):
            raise StudioError(ErrorCode.PROFILE_MISMATCH, "Profile 与 I/O 契约不匹配")
        return profile

    def deployment_profile(
        self, pipeline: Any, profile_name: str = "", filename: str = "",
    ) -> dict[str, Any]:
        """构建一份供预览、运行和保存共用的部署快照。"""
        path = self.managed_path(filename) if filename else None
        managed = self.generated_solutions.get(path.name) if path else None
        if not managed:
            return self.profile_inputs(pipeline, profile_name)
        conf_path = managed["conf_path"]
        if (path.is_symlink() or conf_path.is_symlink()
                or not path.is_file() or not conf_path.is_file()):
            raise StudioError(ErrorCode.REVISION_CONFLICT, "配套 JSON 或 .conf 已被替换或删除，请重新关联", 409)
        raw, conf_raw = path.read_bytes(), conf_path.read_bytes()
        if (revision_for(raw) != managed["pipeline_revision"]
                or revision_for(conf_raw) != managed["conf_revision"]):
            raise StudioError(ErrorCode.REVISION_CONFLICT, "配套 JSON 或 .conf 已被其他编辑器修改，请重新关联", 409)
        if self.io_pairs(json.loads(raw)) != self.io_pairs(pipeline):
            raise StudioError(ErrorCode.DEPLOYMENT_MISMATCH, "已关联方案不能改变 I/O 契约，请另存方案")
        return self.profile_inputs(pipeline, profile_name) if profile_name else copy.deepcopy(managed["profile"])

    def resolve_run_conf(self, conf_path: Path, profile: dict[str, Any]) -> dict[str, Any]:
        depth = max(int(profile.get("batch_size", 1)), int(profile.get("depth", 1)))
        report = self.invoke_tool(["resolve-conf", conf_path.name, "--root",
                                   str(conf_path.parent.resolve()), "--depth", str(depth)])
        if not report.get("ok"):
            raise StudioError(ErrorCode.DEPLOYMENT_VALIDATION_FAILED, json.dumps(report, ensure_ascii=False))
        return report["configuration"]

    @staticmethod
    def demo_command(profile: dict[str, Any], conf_path: Path, output_dir: Path) -> list[str]:
        name = conf_path.stem
        run_profile = copy.deepcopy(profile)
        run_profile["config"] = str(conf_path.resolve())
        for field in ("dataset", "control_file"):
            if field in run_profile:
                run_profile[field] = str(PROJECT_ROOT / run_profile[field])
        output_dir.mkdir(parents=True, exist_ok=True)
        profile_path = output_dir / "demo-profile.json"
        if profile_path.is_symlink():
            raise StudioError(ErrorCode.SYMLINK_REJECTED, "拒绝覆盖符号链接运行 Profile", 409)
        profile_path.write_text(json.dumps({"profiles": {name: run_profile}},
                                          ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return [
            str(DEMO_BINARY), "--profiles-file", str(profile_path), "--profile", name,
            "--output-dir", str(output_dir),
        ]

    def save_solution(self, requested: str, pipeline: Any, profile_name: str) -> dict[str, Any]:
        with self.solution_lock:
            report = self.validate(pipeline)
            if not report.get("ok"):
                raise StudioError(ErrorCode.VALIDATION_FAILED, json.dumps(report, ensure_ascii=False))
            path = self.managed_path(requested)
            conf_path = path.with_suffix(".conf")
            for target in (path, conf_path):
                if target.is_symlink():
                    raise StudioError(ErrorCode.SYMLINK_REJECTED, "拒绝写入符号链接方案")
                if target.exists():
                    raise StudioError(ErrorCode.FILE_EXISTS, f"另存目标已存在：{target.name}", 409)
            profile = self.deployment_profile(pipeline, profile_name, path.name)
            conf = {"pipe_path": path.name}
            encoded = (json.dumps(pipeline, ensure_ascii=False, indent=2) + "\n").encode()
            conf_encoded = (json.dumps(conf, ensure_ascii=False, indent=2) + "\n").encode()
            created = []
            self.config_root.mkdir(parents=True, exist_ok=True)
            try:
                for target, contents in ((path, encoded), (conf_path, conf_encoded)):
                    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
                    created.append(target)
                    with os.fdopen(descriptor, "wb") as stream:
                        stream.write(contents)
                        stream.flush()
                        os.fsync(stream.fileno())
                configuration = self.resolve_run_conf(conf_path, profile)
            except Exception as error:
                for target in created:
                    target.unlink(missing_ok=True)
                if isinstance(error, FileExistsError):
                    raise StudioError(ErrorCode.FILE_EXISTS, "另存目标已存在", 409) from error
                if isinstance(error, StudioError):
                    raise
                raise StudioError(ErrorCode.SAVE_FAILED, str(error), 500) from error
            self.generated_solutions[path.name] = {
                "profile": profile,
                "conf_path": conf_path,
                "pipeline_revision": revision_for(encoded),
                "conf_revision": revision_for(conf_encoded),
            }
            return self.solution_result(path, pipeline, encoded, profile, configuration)

    def solution_result(self, path: Path, pipeline: Any, encoded: bytes, profile: dict[str, Any], configuration: Any) -> dict[str, Any]:
        conf_path = self.generated_solutions[path.name]["conf_path"]
        command_str = ""
        command_warning = ""
        if profile and profile.get("dataset"):
            try:
                command = self.demo_command(profile, conf_path, PROJECT_ROOT / "output" / path.stem)
                command_str = f"cd {shlex.quote(str(PROJECT_ROOT))} && {shlex.join(command)}"
            except (OSError, StudioError) as error:
                command_warning = f"方案文件已保存，但运行命令未生成：{error}"
        return json_result(
            True, filename=path.name, conf_filename=conf_path.name, revision=revision_for(encoded),
            pipeline=pipeline, configuration=configuration,
            command=command_str,
            command_warning=command_warning,
            deployment=self.deployment_info(path.name),
        )

    def associate_deployment(
        self, pipeline_name: str, conf_name: str,
        profile_name: str = "",
    ) -> dict[str, Any]:
        with self.solution_lock:
            pipe_path = self.managed_path(pipeline_name, must_exist=True)
            conf_path = self.managed_path(conf_name, must_exist=True, suffix=".conf")

            pipe_raw = pipe_path.read_bytes()
            conf_raw = conf_path.read_bytes()
            configuration = self.resolve_run_conf(conf_path, {})
            pipeline = json.loads(pipe_raw)
            resolved_pipe = Path(configuration.get("pipeline_path", "")).resolve()
            if resolved_pipe != pipe_path.resolve():
                raise StudioError(
                    ErrorCode.DEPLOYMENT_MISMATCH,
                    f"部署配置指向 {resolved_pipe.name}，与当前方案 {pipe_path.name} 不符",
                    400,
                )

            if profile_name:
                prof = self.profile_inputs(pipeline, profile_name)
            else:
                prof = None
                candidate_error = None
                matching = {candidate["name"] for candidate in self.catalog().get("profiles", [])
                            if candidate.get("io") == self.io_pairs(pipeline)}
                for name in read_json(PROFILE_FILE).get("profiles", {}):
                    if name not in matching:
                        continue
                    try:
                        prof = self.profile_inputs(pipeline, name)
                    except StudioError as error:
                        if error.code != ErrorCode.DEPLOYMENT_VALIDATION_FAILED.name:
                            raise
                        candidate_error = error
                        continue
                    break
                if prof is None:
                    if candidate_error is not None:
                        raise candidate_error
                    raise StudioError(ErrorCode.PROFILE_MISMATCH, "没有与该 I/O 契约匹配的运行 Profile")
            configuration = self.resolve_run_conf(conf_path, prof)
            if pipe_path.read_bytes() != pipe_raw or conf_path.read_bytes() != conf_raw:
                raise StudioError(ErrorCode.REVISION_CONFLICT, "关联期间文件已改变，请重新关联", 409)

            self.generated_solutions[pipe_path.name] = {
                "conf_path": conf_path,
                "conf_revision": revision_for(conf_raw),
                "pipeline_revision": revision_for(pipe_raw),
                "profile": prof,
            }

            return json_result(
                True,
                pipeline_name=pipe_path.name,
                conf_name=conf_path.name,
                conf_revision=revision_for(conf_raw),
                configuration=configuration,
                deployment=self.deployment_info(pipe_path.name),
            )

    def preflight(
        self, pipeline: Any, profile_name: str = "", filename: str = "",
    ) -> dict[str, Any]:
        report = self.validate(pipeline, explain=True)
        if not report.get("ok"):
            return json_result(
                False, summary={
                    "status": "validation_failed",
                    "io": self.io_pairs(pipeline) if isinstance(pipeline, dict) else {},
                    "project_root": str(PROJECT_ROOT),
                    "next_step": "请先修复 Pipeline 校验错误",
                }, validation=report,
            )
        if not profile_name and filename not in self.generated_solutions:
            return json_result(
                True, summary={
                    "status": "no_deployment_conf", "io": self.io_pairs(pipeline),
                    "project_root": str(PROJECT_ROOT),
                    "next_step": "Pipeline 校验通过。请关联部署配置或选择 Profile 进行部署预检。",
                }, validation=report,
            )
        profile = self.deployment_profile(pipeline, profile_name, filename)
        with self.temporary_documents(pipeline, ".studio-preflight-") as (_, staged_conf):
            configuration = self.resolve_run_conf(staged_conf, profile)
            assets = []
            all_assets_ready = True
            for info in configuration.get("model_files", []):
                path = Path(info["resolved"])
                exists = path.is_file()
                all_assets_ready &= exists
                assets.append({"model": info["model"], "path": info["path"],
                               "resolved": str(path),
                               "status": "exists_unverified" if exists else "missing"})
            tools_ready = PIPELINE_TOOL.is_file() and DEMO_BINARY.is_file()
            next_steps = []
            if not all_assets_ready:
                next_steps.append("部分模型资产文件缺失，请检查路径或准备模型。")
            if not DEMO_BINARY.is_file():
                next_steps.append("Demo 可执行文件未就绪，请构建 build/alg_demo。")
            if all_assets_ready and tools_ready:
                next_steps.append("配置与资源预检通过，可执行运行或成套保存。")
            summary = {
                "status": "ready" if all_assets_ready and tools_ready else "attention_required",
                "io": self.io_pairs(pipeline), "project_root": str(PROJECT_ROOT),
                "conf_path": (self.generated_solutions[filename]["conf_path"].name
                              if filename in self.generated_solutions else
                              (profile_name and f"profile:{profile_name}") or ""),
                "assets": assets,
                "tools": {"alg_pipeline_tool": PIPELINE_TOOL.is_file(),
                          "alg_demo": DEMO_BINARY.is_file()},
                "next_step": " ".join(next_steps) if next_steps else "预检完成",
            }
            return json_result(True, summary=summary, configuration=configuration, validation=report)

    def start_run(self, pipeline: Any, profile_name: str,
                  filename: str = "") -> dict[str, Any]:
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError(ErrorCode.VALIDATION_FAILED, json.dumps(report, ensure_ascii=False))
        profile = self.deployment_profile(
            pipeline, profile_name, filename)
        with self.job_lock:
            if any(job["status"] in ("queued", "running") for job in self.jobs.values()):
                raise StudioError(ErrorCode.RUN_BUSY, "同一工作台最多运行一个任务", 409)
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
            args=(job_id, copy.deepcopy(pipeline), profile),
            daemon=True,
        ).start()
        return json_result(True, job_id=job_id, status="queued")

    @staticmethod
    def stop_process(process: subprocess.Popen) -> None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=PROCESS_STOP_TIMEOUT_SECONDS)
        except subprocess.TimeoutExpired:
            pass
        finally:
            # 主进程先退出时，也结束同组中仍在运行的子进程。
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()

    def _run_job(
        self,
        job_id: str,
        pipeline: Any,
        profile: dict[str, Any],
    ) -> None:
        temporary = None
        try:
            temporary_parent = PROJECT_ROOT / "build"
            if not temporary_parent.is_dir():
                temporary_parent = PROJECT_ROOT
            temporary = tempfile.TemporaryDirectory(prefix=".studio-run-", dir=temporary_parent)
            temp_root = Path(temporary.name)
            with self.temporary_documents(pipeline, ".studio-run-") as (_, conf_path):
                configuration = self.resolve_run_conf(conf_path, profile)
                output_dir = temp_root / "results"
                args = self.demo_command(profile, conf_path, output_dir)
                timeout = SMOKE_RUN_TIMEOUT_SECONDS if profile.get("suite", "smoke") == "smoke" else REAL_RUN_TIMEOUT_SECONDS
                process = None
                timed_out = False
                with tempfile.TemporaryFile() as log:
                    try:
                        with self.job_lock:
                            job = self.jobs[job_id]
                            job["configuration"] = configuration
                            if job["cancel_requested"]:
                                job["status"] = "cancelled"
                                job["finished_at"] = time.time()
                                return
                            if not DEMO_BINARY.is_file():
                                raise StudioError(ErrorCode.DEMO_NOT_BUILT, "请先构建 build/alg_demo", 503)
                            # 检查取消、启动和登记进程共用锁，取消不会落在登记之前。
                            process = subprocess.Popen(
                                args, cwd=PROJECT_ROOT, stdout=log, stderr=subprocess.STDOUT,
                                start_new_session=True,
                            )
                            job["process"] = process
                            job["status"] = "running"
                            job["started_at"] = time.time()
                        deadline = time.monotonic() + timeout
                        while True:
                            with self.job_lock:
                                cancelled = self.jobs[job_id]["cancel_requested"]
                            remaining = deadline - time.monotonic()
                            if cancelled or remaining <= 0:
                                timed_out = not cancelled
                                break
                            try:
                                process.wait(timeout=min(.2, remaining))
                                break
                            except subprocess.TimeoutExpired:
                                continue
                    finally:
                        if process is not None:
                            with self.job_lock:
                                cancelled = self.jobs[job_id]["cancel_requested"]
                            if cancelled or timed_out or process.poll() is None:
                                self.stop_process(process)
                            log.seek(0, os.SEEK_END)
                            log.seek(max(0, log.tell() - MAX_LOG_BYTES))
                            with self.job_lock:
                                job = self.jobs[job_id]
                                job["logs"] = log.read().decode("utf-8", errors="replace")
                                job["exit_code"] = process.returncode
                if timed_out:
                    raise StudioError(ErrorCode.RUN_TIMEOUT, f"运行超过 {timeout} 秒", 408)
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
                    "code": getattr(error, "code", ErrorCode.INTERNAL_EXCEPTION.name),
                    "message": str(error),
                }
                job["finished_at"] = time.time()
                job["process"] = None
        finally:
            if temporary is not None:
                temporary.cleanup()

    def run_status(self, job_id: str) -> dict[str, Any]:
        with self.job_lock:
            job = self.jobs.get(job_id)
            if not job:
                raise StudioError(ErrorCode.RUN_NOT_FOUND, job_id, 404)
            public = {key: value for key, value in job.items() if key != "process"}
        return json_result(True, job=public)

    def cancel_run(self, job_id: str) -> dict[str, Any]:
        with self.job_lock:
            job = self.jobs.get(job_id)
            if not job:
                raise StudioError(ErrorCode.RUN_NOT_FOUND, job_id, 404)
            if job["status"] in ("completed", "failed", "cancelled"):
                return json_result(True, job_id=job_id, status=job["status"])
            job["cancel_requested"] = True
        # 运行线程执行终止和回收，HTTP 请求无需等待退出。
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
            if method in ("POST", "PUT", "DELETE"):
                body = self._body()
            if method == "GET" and path == "/api/catalog":
                payload = service.catalog()
            elif method == "GET" and path == "/api/assets":
                payload = service.assets()
            elif method == "POST" and path == "/api/selection":
                payload = service.verify_selection(body.get("pipeline"), body.get("variant", ""), body.get("filename", ""))
            elif method == "GET" and path == "/api/pipelines":
                payload = service.pipelines()
            elif method == "GET" and path == "/api/pipeline":
                payload = service.open_pipeline(query.get("filename", [""])[0])
            elif method == "GET" and path == "/api/initial":
                payload = service.initial_pipeline()
            elif method == "GET" and path.startswith("/api/runs/"):
                payload = service.run_status(path.rsplit("/", 1)[-1])
            elif method == "POST" and path == "/api/validate":
                payload = service.validate(
                    body.get("pipeline"), body.get("explain", False)
                )
            elif method == "POST" and path == "/api/fixes/preview":
                payload = service.preview_fix(
                    body.get("pipeline"),
                    body.get("patch", []),
                    body.get("revision"),
                    body.get("tool_fingerprint"),
                )
            elif method == "POST" and path == "/api/authoring/preview":
                payload = service.preview_authoring(body)
            elif method == "POST" and path == "/api/preflight":
                payload = service.preflight(
                    body.get("pipeline"),
                    profile_name=body.get("profile", ""),
                    filename=body.get("filename", ""),
                )
            elif method == "POST" and path == "/api/deployment/associate":
                payload = service.associate_deployment(
                    body.get("filename", ""),
                    body.get("conf_name", ""),
                    profile_name=body.get("profile", ""),
                )
            elif method == "POST" and path == "/api/init":

                payload = service.init_pipeline(
                    body.get("input"), body.get("output"), body.get("profile", "")
                )
            elif method == "POST" and path == "/api/pipelines":
                payload = service.save_pipeline(
                    body.get("filename", ""), body.get("pipeline"), None, save_as=True
                )
            elif method == "POST" and path == "/api/solutions":
                payload = service.save_solution(
                    body.get("filename", ""), body.get("pipeline"), body.get("profile", "")
                )
            elif method == "PUT" and path == "/api/pipeline":
                payload = service.save_pipeline(
                    body.get("filename", ""),
                    body.get("pipeline"),
                    body.get("revision"),
                    save_as=False,
                    profile_name=body.get("profile", ""),
                )
            elif method == "POST" and path == "/api/runs":
                payload = service.start_run(
                    body.get("pipeline"), body.get("profile", ""),
                    filename=body.get("filename", ""),
                )
            elif method == "DELETE" and path.startswith("/api/runs/"):
                payload = service.cancel_run(path.rsplit("/", 1)[-1])
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

        def do_DELETE(self) -> None:
            self._respond("DELETE")

        def _respond(self, method: str) -> None:
            try:
                self._dispatch(method)
            except StudioError as error:
                detail: Any = str(error)
                if error.code == ErrorCode.VALIDATION_FAILED.name:
                    detail = json.loads(str(error))
                self._send(
                    error.status,
                    json_result(False, error={"code": error.code, "message": detail}),
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
    inputs, outputs = SELECTION.pipeline_io(pipeline)
    pairs = lambda items: ", ".join(item["type"] + "/" + item["name"] for item in items)
    print(f"io: {pairs(inputs)} -> {pairs(outputs)}")
    for model in pipeline.get("models", []):
        print(f"  model {model.get('name', '<missing-name>')}: {model.get('type', 'unknown')}"
              f" / {model.get('backend', {}).get('type', 'unknown')} / {model.get('file', '')}")
    for index, node in enumerate(pipeline.get("pipeline", [])):
        name = node.get("name", "<missing-name>")
        print(f"  [{index}] {name}: {node.get('type', 'unknown')}")
        mapping = json.dumps(node["inputs"], ensure_ascii=False) if "inputs" in node else "<未声明>"
        print(f"      inputs: {mapping}")
        print(f"      depends_on: {json.dumps(node.get('depends_on', []), ensure_ascii=False)} (额外顺序)")
    for output in outputs:
        print(f"  output {output['type']}/{output['name']}: "
              f"{json.dumps(output.get('inputs', {}), ensure_ascii=False)}")
    print()

def read_pipeline_file(path: Path) -> dict[str, Any]:
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
    if not isinstance(pipeline, dict) or not isinstance(pipeline.get("pipeline"), list):
        raise StudioError(ErrorCode.INVALID_PIPELINE_DOCUMENT, "方案必须是包含 pipeline 数组的 JSON 对象")
    return pipeline


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
