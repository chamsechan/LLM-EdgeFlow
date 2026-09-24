#!/usr/bin/env python3
"""Terminal viewer and local Pipeline Studio server for LLM-EdgeFlow."""

from __future__ import annotations

import argparse
import copy
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import re
import shlex
import shutil
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

MANAGED_NAME = re.compile(r"^pipeline_[a-z0-9_]+\.json$")
MAX_LOG_BYTES = 2 * 1024 * 1024
MAX_DOCUMENT_BYTES = 4 * 1024 * 1024
_AUTHORING_UNSET = object()


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


def apply_json_patch(doc: Any, patch: list[dict[str, Any]]) -> Any:
    doc = copy.deepcopy(doc)

    def parse_pointer(p: str) -> list[str]:
        if not p.startswith("/"):
            if not p:
                return []
            raise ValueError(f"Invalid JSON Pointer: {p}")
        parts = p[1:].split("/")
        return [part.replace("~1", "/").replace("~0", "~") for part in parts]

    def get_parent_and_key(d: Any, parts: list[str]) -> tuple[Any, str | int]:
        curr = d
        for part in parts[:-1]:
            if isinstance(curr, list):
                curr = curr[int(part)]
            elif isinstance(curr, dict):
                curr = curr[part]
            else:
                raise KeyError(f"Invalid path traversal: {part}")
        key: str | int = parts[-1]
        if isinstance(curr, list) and key != "-":
            key = int(key)
        return curr, key

    for op_obj in patch:
        op = op_obj["op"]
        path_parts = parse_pointer(op_obj["path"])
        if op == "test":
            curr, key = get_parent_and_key(doc, path_parts)
            val = curr[key]
            if val != op_obj["value"]:
                raise ValueError(
                    f"Patch test failed at {op_obj['path']}: expected {op_obj['value']!r}, got {val!r}"
                )
        elif op == "add":
            if not path_parts:
                doc = copy.deepcopy(op_obj["value"])
                continue
            curr, key = get_parent_and_key(doc, path_parts)
            val = copy.deepcopy(op_obj["value"])
            if isinstance(curr, list):
                if key == "-" or key == len(curr):
                    curr.append(val)
                else:
                    curr.insert(int(key), val)
            elif isinstance(curr, dict):
                curr[str(key)] = val
        elif op == "remove":
            curr, key = get_parent_and_key(doc, path_parts)
            if isinstance(curr, list):
                del curr[int(key)]
            elif isinstance(curr, dict):
                del curr[str(key)]
        elif op == "replace":
            curr, key = get_parent_and_key(doc, path_parts)
            val = copy.deepcopy(op_obj["value"])
            if isinstance(curr, list):
                curr[int(key)] = val
            elif isinstance(curr, dict):
                curr[str(key)] = val
        elif op == "move":
            from_parts = parse_pointer(op_obj["from"])
            from_parent, from_key = get_parent_and_key(doc, from_parts)
            if isinstance(from_parent, list):
                val = from_parent.pop(int(from_key))
            elif isinstance(from_parent, dict):
                val = from_parent.pop(str(from_key))
            curr, key = get_parent_and_key(doc, path_parts)
            if isinstance(curr, list):
                if key == "-" or key == len(curr):
                    curr.append(val)
                else:
                    curr.insert(int(key), val)
            elif isinstance(curr, dict):
                curr[str(key)] = val
        elif op == "copy":
            from_parts = parse_pointer(op_obj["from"])
            from_parent, from_key = get_parent_and_key(doc, from_parts)
            val = copy.deepcopy(from_parent[from_key])
            curr, key = get_parent_and_key(doc, path_parts)
            if isinstance(curr, list):
                if key == "-" or key == len(curr):
                    curr.append(val)
                else:
                    curr.insert(int(key), val)
            elif isinstance(curr, dict):
                curr[str(key)] = val
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
    """State and filesystem boundary behind /api/v1."""

    def __init__(self, config_root: Path = CONFIG_ROOT, initial: Path | None = None):
        self.config_root = config_root.resolve()
        self.jobs: dict[str, dict[str, Any]] = {}
        self.job_lock = threading.Lock()
        self.solution_lock = threading.Lock()
        # Ownership is limited to pairs created by this server session.
        self.generated_solutions: dict[str, dict[str, Any]] = {}
        self.initial_document = None
        if initial is not None:
            pipeline = read_pipeline_file(initial)
            self.initial_document = json_result(
                True, filename=initial.name, revision="", pipeline=pipeline, imported=True
            )
            # Only the existing managed file contract grants overwrite access.
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
                    "io_binding": SELECTION.pipeline_binding(pipeline),
                    "revision": revision_for(raw),
                }
            )
        confs = [p.name for p in sorted(self.config_root.glob("*.conf")) if p.is_file()]
        return json_result(True, pipelines=items, deployments=confs)

    def save_targets(self, path: Path) -> list[str]:
        if path.name in self.generated_solutions:
            conf_name = self.generated_solutions[path.name]["conf_path"].name
            return [path.name, conf_name]
        return [path.name]

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
            save_targets=self.save_targets(path),
            pipeline=pipeline,
            deployment=self.deployment_info(path.name),
        )

    def deployment_info(self, filename: str) -> dict[str, Any] | None:
        managed = self.generated_solutions.get(filename)
        if not managed:
            return None
        return {"conf_name": managed["conf_path"].name,
                "conf_revision": managed["conf_revision"],
                "model_root": managed["model_root"]}

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

    def catalog(self, io_binding: str = "") -> dict[str, Any]:
        args = ["catalog"]
        if io_binding:
            args.extend(["--io-binding", io_binding])
        return self.invoke_tool(args)

    def assets(self) -> dict[str, Any]:
        return {"ok": True, **SELECTION.asset_catalog()}

    def verify_selection(self, pipeline: Any, variant: str = "", model_root: str = "models") -> dict[str, Any]:
        try:
            root = SELECTION.within(PROJECT_ROOT, model_root)
            return SELECTION.inspect_selection(pipeline, PIPELINE_TOOL, root, variant=variant or None, pipeline_root=PROJECT_ROOT)
        except (ValueError, KeyError, TypeError, OSError, subprocess.SubprocessError) as error:
            raise StudioError("SELECTION_CHECK_FAILED", str(error)) from error

    def profiles(self) -> dict[str, Any]:
        root = read_json(PROFILE_FILE)
        profiles = []
        for name, profile in sorted(root.get("profiles", {}).items()):
            try:
                DEMO_PROFILE.validate_profile_fields(profile)
            except ValueError as error:
                raise StudioError("INVALID_PROFILE", f"Profile '{name}': {error}") from error
            item = dict(profile)
            item["name"] = name
            profiles.append(item)
        return json_result(True, profiles=profiles)

    def get_tool_fingerprint(self) -> str:
        return get_tool_fingerprint()

    def validate(self, pipeline: Any, explain: bool = False) -> dict[str, Any]:
        args = ["validate", "--stdin"]
        if explain:
            args.append("--explain")
        fingerprint = self.get_tool_fingerprint() if explain else None
        report = self.invoke_tool(args, pipeline)
        if explain and isinstance(report, dict):
            if fingerprint != self.get_tool_fingerprint():
                raise StudioError("TOOL_OUTDATED", "校验期间工具已更新，请重新校验", 409)
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
            raise StudioError("REVISION_CONFLICT", "必须指定预期修订版本，草稿可能已变更", 409)
        raw = json.dumps(pipeline, sort_keys=True).encode("utf-8")
        current_rev = revision_for(raw)
        if current_rev != expected_revision:
            raise StudioError("REVISION_CONFLICT", "草稿已变更，候选失效", 409)
        current_fp = self.get_tool_fingerprint()
        if not tool_fingerprint or current_fp != tool_fingerprint:
            raise StudioError("TOOL_OUTDATED", "底层校验工具已重建或更新，候选失效", 409)
        try:
            patched = apply_json_patch(pipeline, patch)
        except Exception as error:
            raise StudioError(
                "PATCH_APPLICATION_FAILED", f"补丁应用失败: {error}"
            ) from error
        report = self.validate(patched, explain=True)
        if current_fp != self.get_tool_fingerprint():
            raise StudioError("TOOL_OUTDATED", "修复预览期间工具已更新，请重新校验", 409)
        return json_result(
            True,
            patched=patched,
            report=report,
            revision=revision_for(
                json.dumps(patched, sort_keys=True).encode("utf-8")
            ),
            tool_fingerprint=current_fp,
        )

    def preview_authoring(
        self,
        pipeline: Any,
        operation: Any = _AUTHORING_UNSET,
        operations: Any = _AUTHORING_UNSET,
        require_valid: bool = False,
        expected_revision: str | None = None,
        tool_fingerprint: str | None = None,
    ) -> dict[str, Any]:
        if not expected_revision:
            raise StudioError("REVISION_CONFLICT", "必须指定预期修订版本，草稿可能已变更", 409)
        raw = json.dumps(pipeline, sort_keys=True).encode("utf-8")
        if revision_for(raw) != expected_revision:
            raise StudioError("REVISION_CONFLICT", "草稿已变更，操作失效", 409)
        current_fp = self.get_tool_fingerprint()
        if not tool_fingerprint or current_fp != tool_fingerprint:
            raise StudioError("TOOL_OUTDATED", "操作期间工具已更新，请重新加载", 409)

        req: dict[str, Any] = {
            "schema_version": 1,
            "pipeline": pipeline,
            "require_valid": require_valid,
        }
        if operations is not _AUTHORING_UNSET:
            req["operations"] = operations
        if operation is not _AUTHORING_UNSET:
            req["operation"] = operation

        res = self.invoke_tool(["edit", "--stdin"], req)
        if self.get_tool_fingerprint() != current_fp:
            raise StudioError("TOOL_OUTDATED", "操作期间工具已更新，请重新加载", 409)
        if isinstance(res, dict):
            res["tool_fingerprint"] = current_fp
            if res.get("pipeline"):
                res["revision"] = revision_for(
                    json.dumps(res["pipeline"], sort_keys=True).encode("utf-8")
                )
        return res


    def init_pipeline(
        self, io_binding: str, profile: str = "", empty: bool = False
    ) -> dict[str, Any]:
        args = ["init", "--io-binding", io_binding]
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
        profile_name: str = "",
        model_root: str = "models",
    ) -> dict[str, Any]:
        path = self.managed_path(requested)
        if not save_as and path.name in self.generated_solutions:
            return self.update_solution(path, pipeline, expected_revision, profile_name, model_root)
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
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
            save_targets=self.save_targets(path),
            pipeline=pipeline,
            deployment=self.deployment_info(path.name),
        )

    def update_solution(self, path: Path, pipeline: Any, expected_revision: str | None,
                        profile_name: str = "", model_root: str = "models") -> dict[str, Any]:
        with self.solution_lock:
            managed = self.generated_solutions[path.name]
            conf_path = managed["conf_path"]

            def check_revisions() -> tuple[bytes, bytes]:
                if path.is_symlink() or conf_path.is_symlink() or not path.is_file() or not conf_path.is_file():
                    raise StudioError("REVISION_CONFLICT", "配套 JSON 或 .conf 已被替换或删除，请重新检查文件", 409)
                raw, conf_raw = path.read_bytes(), conf_path.read_bytes()
                if revision_for(raw) != expected_revision or revision_for(conf_raw) != managed["conf_revision"]:
                    raise StudioError("REVISION_CONFLICT", "配套 JSON 或 .conf 已被其他编辑器修改，请重新检查文件", 409)
                return raw, conf_raw

            old_json, _ = check_revisions()
            profile, conf = self.deployment_candidate(
                pipeline, profile_name, model_root, path.name)
            report = self.validate(pipeline)
            if not report.get("ok"):
                raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
            encoded = (json.dumps(pipeline, ensure_ascii=False, indent=2) + "\n").encode()
            conf_encoded = (json.dumps(conf, ensure_ascii=False, indent=2) + "\n").encode()
            staging = Path(tempfile.mkdtemp(prefix=".studio-save-", dir=self.config_root))
            preserve_backup = False
            try:
                staged_json = staging / "pipeline.json"
                staged_conf = staging / "pipeline.conf"
                backup_json = staging / "previous.json"
                staged_json.write_bytes(encoded)
                staged_conf_data = {"pipe_path": staged_json.name}
                staged_conf.write_text(json.dumps(staged_conf_data, ensure_ascii=False, indent=2))
                configuration = self.resolve_run_conf(staged_conf, profile)

                # Native validation used the staged JSON; installed paths have
                # the same model mappings and normalized node configuration.
                configuration["conf_path"] = str(conf_path)
                configuration["pipeline_path"] = str(path)
                staged_conf.write_bytes(conf_encoded)
                backup_json.write_bytes(old_json)
                check_revisions()
                os.replace(staged_json, path)
                try:
                    os.replace(staged_conf, conf_path)
                except Exception:
                    try:
                        os.replace(backup_json, path)
                    except OSError as rollback_error:
                        preserve_backup = True
                        raise StudioError("SAVE_ROLLBACK_FAILED", f"保存失败，旧 JSON 备份保留在 {backup_json}: {rollback_error}", 500) from rollback_error
                    raise
                managed["conf_revision"] = revision_for(conf_encoded)
                managed["pipeline_revision"] = revision_for(encoded)
                managed["profile"] = profile
                managed["model_root"] = model_root
                return self.solution_result(path, pipeline, conf, encoded, profile, managed["model_root"], configuration)
            except StudioError:
                raise
            except Exception as error:
                raise StudioError("SAVE_FAILED", str(error), 500) from error
            finally:
                if not preserve_backup:
                    shutil.rmtree(staging)

    def profile_inputs(self, pipeline: Any, profile_name: str) -> tuple[dict, Any]:
        profiles = read_json(PROFILE_FILE).get("profiles", {})
        if profile_name not in profiles:
            raise StudioError("UNKNOWN_PROFILE", profile_name)
        profile = profiles[profile_name]
        try:
            DEMO_PROFILE.validate_profile_fields(profile)
        except ValueError as error:
            raise StudioError("INVALID_PROFILE", f"Profile '{profile_name}': {error}") from error
        profile_conf = PROJECT_ROOT / profile["config"]
        configuration = self.resolve_run_conf(profile_conf, profile)
        original = read_json(Path(configuration["pipeline_path"]))
        if SELECTION.pipeline_binding(original) != SELECTION.pipeline_binding(pipeline):
            raise StudioError("PROFILE_MISMATCH", "Profile 与I/O 契约不匹配")
        outputs = original.get("deployment", {}).get("io", {}).get("out_mem", {})
        return copy.deepcopy(profile), copy.deepcopy(outputs)

    def run_conf(self, pipeline: Any, outputs: Any, pipe_path: Path, model_root: str) -> dict[str, Any]:
        if not isinstance(model_root, str) or not model_root or Path(model_root).is_absolute():
            raise StudioError("INVALID_MODEL_ROOT", "模型目录必须是项目内的相对路径（例如 models 或 .）")
        try:
            return SELECTION.build_run_conf(pipeline, outputs, pipe_path, model_root, PROJECT_ROOT)
        except (ValueError, TypeError, KeyError) as error:
            raise StudioError("INVALID_DEPLOYMENT_PATH", str(error)) from error

    def deployment_candidate(
        self, pipeline: Any, profile_name: str = "", model_root: str = "models",
        filename: str = "", conf_name: str = "",
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        """Build one deployment snapshot shared by preview, run and save."""
        if conf_name:
            requested_conf = self.managed_conf_path(conf_name, must_exist=True)
            if not filename:
                matches = [name for name, info in self.generated_solutions.items()
                           if info["conf_path"] == requested_conf]
                if len(matches) != 1:
                    raise StudioError("DEPLOYMENT_NOT_ASSOCIATED", "请先明确关联当前方案与部署配置")
                filename = matches[0]
        managed = None
        path = None
        if filename:
            path = self.managed_path(filename)
            managed = self.generated_solutions.get(path.name)
        if conf_name and (not managed or managed["conf_path"] != requested_conf):
            raise StudioError("DEPLOYMENT_NOT_ASSOCIATED", "部署配置未关联到当前方案")
        if not managed:
            profile, outputs = self.profile_inputs(pipeline, profile_name)
            return profile, self.run_conf(pipeline, outputs, path or PROJECT_ROOT / "build/pipeline.json", model_root)
        conf_path = managed["conf_path"]
        if (path.is_symlink() or conf_path.is_symlink()
                or not path.is_file() or not conf_path.is_file()):
            raise StudioError("REVISION_CONFLICT", "配套 JSON 或 .conf 已被替换或删除，请重新关联", 409)
        raw, conf_raw = path.read_bytes(), conf_path.read_bytes()
        if (revision_for(raw) != managed["pipeline_revision"]
                or revision_for(conf_raw) != managed["conf_revision"]):
            raise StudioError("REVISION_CONFLICT", "配套 JSON 或 .conf 已被其他编辑器修改，请重新关联", 409)
        original = json.loads(raw)
        if SELECTION.pipeline_binding(original) != SELECTION.pipeline_binding(pipeline):
            raise StudioError("DEPLOYMENT_MISMATCH", "已关联方案不能改变 I/O 契约，请另存方案")
        profile = self.profile_inputs(pipeline, profile_name)[0] if profile_name else copy.deepcopy(managed["profile"])
        if "io" in original.get("deployment", {}):
            pipeline.setdefault("deployment", {})["io"] = copy.deepcopy(original["deployment"]["io"])
        return profile, {"pipe_path": path.name}

    def resolve_run_conf(self, conf_path: Path, profile: dict[str, Any]) -> dict[str, Any]:
        depth = max(int(profile.get("batch_size", 1)), int(profile.get("depth", 1)))
        report = self.invoke_tool(["resolve-conf", str(conf_path.relative_to(PROJECT_ROOT)), "--root", str(PROJECT_ROOT), "--depth", str(depth)])
        if not report.get("ok"):
            raise StudioError("DEPLOYMENT_VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
        return report["configuration"]

    @staticmethod
    def demo_command(profile: dict[str, Any], conf_path: Path, output_dir: Path, configuration: dict[str, Any]) -> list[str]:
        # Snapshot only settings previously passed to Demo, not runtime Control.
        name = configuration["biz_name"]
        run_profile = {
            "config": str(conf_path),
            "dataset": str(PROJECT_ROOT / profile["dataset"]),
        }
        for field in ("batch_size", "device_id", "chip", "depth"):
            if field in profile:
                run_profile[field] = profile[field]
        output_dir.mkdir(parents=True, exist_ok=True)
        profile_path = output_dir / "demo-profile.json"
        if profile_path.is_symlink():
            raise StudioError("SYMLINK_REJECTED", "拒绝覆盖符号链接运行 Profile", 409)
        profile_path.write_text(json.dumps({"schema_version": 2, "profiles": {name: run_profile}},
                                          ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        return [
            str(DEMO_BINARY), "--profiles-file", str(profile_path), "--profile", name,
            "--output-dir", str(output_dir),
        ]

    def save_solution(self, requested: str, pipeline: Any, profile_name: str, model_root: str = "models") -> dict[str, Any]:
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
        path = self.managed_path(requested)
        conf_path = path.with_suffix(".conf")
        for target in (path, conf_path):
            if target.is_symlink():
                raise StudioError("SYMLINK_REJECTED", "拒绝写入符号链接方案")
            if target.exists():
                raise StudioError("FILE_EXISTS", f"另存目标已存在：{target.name}", 409)
        profile, conf = self.deployment_candidate(pipeline, profile_name, model_root, path.name)
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
                raise StudioError("FILE_EXISTS", "另存目标已存在", 409) from error
            if isinstance(error, StudioError):
                raise
            raise StudioError("SAVE_FAILED", str(error), 500) from error
        self.generated_solutions[path.name] = {
            "profile": profile, "model_root": model_root,
            "conf_path": conf_path,
            "pipeline_revision": revision_for(encoded),
            "conf_revision": revision_for(conf_encoded),
        }
        return self.solution_result(path, pipeline, conf, encoded, profile, model_root, configuration)

    def solution_result(self, path: Path, pipeline: Any, conf: Any, encoded: bytes, profile: dict[str, Any], model_root: str, configuration: Any) -> dict[str, Any]:
        conf_path = self.generated_solutions[path.name]["conf_path"]
        command_str = ""
        if profile and profile.get("dataset"):
            command = self.demo_command(profile, conf_path.relative_to(PROJECT_ROOT), PROJECT_ROOT / "output" / path.stem, configuration)
            command_str = f"cd {shlex.quote(str(PROJECT_ROOT))} && {shlex.join(command)}"
        return json_result(
            True, filename=path.name, conf_filename=conf_path.name, revision=revision_for(encoded),
            pipeline=pipeline, conf=conf, model_root=model_root, configuration=configuration,
            save_targets=self.save_targets(path),
            command=command_str,
            deployment=self.deployment_info(path.name),
        )

    def managed_conf_path(self, requested: str, must_exist: bool = False) -> Path:
        path = Path(requested)
        if path.is_absolute() or len(path.parts) not in (1, 2):
            raise StudioError("INVALID_DEPLOYMENT_PATH", "只允许 configs 下的部署配置")
        if len(path.parts) == 2 and path.parts[0] != "configs":
            raise StudioError("INVALID_DEPLOYMENT_PATH", "路径必须位于 configs 目录")
        filename = path.name
        if not re.fullmatch(r"^pipeline_[a-z0-9_]+\.conf$", filename):
            raise StudioError(
                "INVALID_DEPLOYMENT_NAME",
                "文件名必须匹配 pipeline_[a-z0-9_]+.conf",
            )
        candidate = self.config_root / filename
        if candidate.exists() or candidate.is_symlink():
            if candidate.is_symlink():
                raise StudioError("SYMLINK_REJECTED", "拒绝读写符号链接部署配置")
            if candidate.resolve().parent != self.config_root:
                raise StudioError("PATH_ESCAPE", "部署配置路径逃逸 configs 目录")
        elif must_exist:
            raise StudioError("DEPLOYMENT_NOT_FOUND", filename, 404)
        return candidate

    def associate_deployment(
        self, pipeline_name: str, conf_name: str, model_root: str = "models",
        profile_name: str = "",
    ) -> dict[str, Any]:
        pipe_path = self.managed_path(pipeline_name, must_exist=True)
        conf_path = self.managed_conf_path(conf_name, must_exist=True)

        pipe_raw = pipe_path.read_bytes()
        conf_raw = conf_path.read_bytes()
        try:
            pipeline = json.loads(pipe_raw)
            conf = json.loads(conf_raw)
        except json.JSONDecodeError as error:
            raise StudioError("INVALID_JSON", str(error)) from error

        if not isinstance(conf, dict) or "pipe_path" not in conf or not isinstance(conf["pipe_path"], str):
            raise StudioError("INVALID_DEPLOYMENT_CONFIG", "部署配置缺少 pipe_path")

        report = self.invoke_tool([
            "resolve-conf",
            str(conf_path.relative_to(PROJECT_ROOT)),
            "--root",
            str(PROJECT_ROOT),
        ])
        if not report.get("ok"):
            raise StudioError("DEPLOYMENT_VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))

        configuration = report.get("configuration", {})
        resolved_pipe = Path(configuration.get("pipeline_path", "")).resolve()
        if resolved_pipe != pipe_path.resolve():
            raise StudioError(
                "DEPLOYMENT_MISMATCH",
                f"部署配置指向 {resolved_pipe.name}，与当前方案 {pipe_path.name} 不符",
                400,
            )

        if profile_name:
            prof, _ = self.profile_inputs(pipeline, profile_name)
        else:
            prof = None
            for name in read_json(PROFILE_FILE).get("profiles", {}):
                try:
                    prof, _ = self.profile_inputs(pipeline, name)
                    break
                except StudioError as error:
                    if error.code != "PROFILE_MISMATCH":
                        raise
            if prof is None:
                raise StudioError("PROFILE_MISMATCH", "没有与该业务契约匹配的运行 Profile")
        configuration = self.resolve_run_conf(conf_path, prof)
        if pipe_path.read_bytes() != pipe_raw or conf_path.read_bytes() != conf_raw:
            raise StudioError("REVISION_CONFLICT", "关联期间文件已改变，请重新关联", 409)

        with self.solution_lock:
            self.generated_solutions[pipe_path.name] = {
                "conf_path": conf_path,
                "conf_revision": revision_for(conf_raw),
                "pipeline_revision": revision_for(pipe_raw),
                "model_root": model_root,
                "profile": prof,
                "is_associated": True,
            }

        return json_result(
            True,
            pipeline_name=pipe_path.name,
            conf_name=conf_path.name,
            conf_revision=revision_for(conf_raw),
            save_targets=self.save_targets(pipe_path),
            configuration=configuration,
            deployment=self.deployment_info(pipe_path.name),
        )

    def preflight(
        self,
        pipeline: Any,
        profile_name: str = "",
        conf_name: str = "",
        model_root: str = "models",
        filename: str = "",
    ) -> dict[str, Any]:
        report = self.validate(pipeline, explain=True)
        if not report.get("ok"):
            return json_result(
                False,
                summary={
                    "status": "validation_failed",
                    "io_binding": SELECTION.pipeline_binding(pipeline) if isinstance(pipeline, dict) else "",
                    "project_root": str(PROJECT_ROOT),
                    "next_step": "请先修复 Pipeline 校验错误",
                },
                validation=report,
            )

        staging_dir = PROJECT_ROOT / "build" if (PROJECT_ROOT / "build").is_dir() else PROJECT_ROOT
        staging = Path(tempfile.mkdtemp(prefix=".studio-preflight-", dir=staging_dir))
        try:
            staged_pipe = staging / "pipeline.json"
            staged_pipe.write_text(json.dumps(pipeline, ensure_ascii=False, indent=2))
            staged_conf = staging / "pipeline.conf"

            if not profile_name and not conf_name and filename not in self.generated_solutions:
                return json_result(
                    True,
                    summary={
                        "status": "no_deployment_conf",
                        "io_binding": SELECTION.pipeline_binding(pipeline),
                        "project_root": str(PROJECT_ROOT),
                        "pipeline_snapshot": {
                            "node_count": len(pipeline.get("pipeline", [])),
                            "model_count": len(pipeline.get("models", [])),
                        },
                        "next_step": "Pipeline 校验通过。请关联部署配置或选择 Profile 进行部署预检。",
                    },
                    validation=report,
                )

            profile_obj, conf_data = self.deployment_candidate(
                pipeline, profile_name, model_root, filename, conf_name)
            staged_pipe.write_text(json.dumps(pipeline, ensure_ascii=False, indent=2), encoding="utf-8")
            staged_conf_data = {"pipe_path": staged_pipe.name}
            staged_conf.write_text(json.dumps(staged_conf_data, ensure_ascii=False, indent=2))
            configuration = self.resolve_run_conf(staged_conf, profile_obj)

            assets_status = []
            all_assets_ready = True
            for mpath_info in configuration.get("model_paths", []):
                res_path = Path(mpath_info.get("resolved", ""))
                status = "missing"
                if res_path.is_file():
                    status = "exists_unverified"
                else:
                    all_assets_ready = False
                assets_status.append({
                    "model_id": mpath_info.get("model_id", ""),
                    "resolved_path": str(res_path),
                    "source": mpath_info.get("source", ""),
                    "status": status,
                })

            tools_ready = PIPELINE_TOOL.is_file() and DEMO_BINARY.is_file()
            overall_status = "ready" if (all_assets_ready and tools_ready) else "attention_required"

            next_steps = []
            if not all_assets_ready:
                next_steps.append("部分模型资产文件缺失，请检查路径或准备模型。")
            if not DEMO_BINARY.is_file():
                next_steps.append("Demo 可执行文件未就绪，请构建 build/alg_demo。")
            if all_assets_ready and tools_ready:
                next_steps.append("配置与资源预检通过，可执行运行或成套保存。")

            summary = {
                "status": overall_status,
                "biz_name": configuration["biz_name"],
                "io_binding": configuration["io_binding"],
                "project_root": str(PROJECT_ROOT),
                "pipeline_snapshot": {
                    "node_count": len(pipeline.get("pipeline", [])),
                    "model_count": len(pipeline.get("models", [])),
                },
                "conf_path": (self.generated_solutions[filename]["conf_path"].name
                              if filename in self.generated_solutions else conf_name or (profile_name and f"profile:{profile_name}") or ""),
                "model_paths": configuration.get("model_paths", []),
                "output_pools": configuration.get("output_pools", {}),
                "effective_pipeline": configuration.get("effective_pipeline", {}),
                "assets": assets_status,
                "tools": {
                    "alg_pipeline_tool": PIPELINE_TOOL.is_file(),
                    "alg_demo": DEMO_BINARY.is_file(),
                },
                "next_step": " ".join(next_steps) if next_steps else "预检完成",
            }
            return json_result(
                True,
                summary=summary,
                configuration=configuration,
                validation=report,
            )
        finally:
            shutil.rmtree(staging, ignore_errors=True)


    def start_run(self, pipeline: Any, profile_name: str, model_root: str = "models",
                  filename: str = "", conf_name: str = "") -> dict[str, Any]:
        report = self.validate(pipeline)
        if not report.get("ok"):
            raise StudioError("VALIDATION_FAILED", json.dumps(report, ensure_ascii=False))
        profile, conf = self.deployment_candidate(
            pipeline, profile_name, model_root, filename, conf_name)
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
            args=(job_id, copy.deepcopy(pipeline), profile, conf),
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
        temporary = None
        try:
            temporary_parent = PROJECT_ROOT / "build"
            if not temporary_parent.is_dir():
                temporary_parent = PROJECT_ROOT
            temporary = tempfile.TemporaryDirectory(prefix=".studio-run-", dir=temporary_parent)
            temp_root = Path(temporary.name)
            pipeline_path = temp_root / "pipeline.json"
            pipeline_path.write_text(
                json.dumps(pipeline, ensure_ascii=False, indent=2), encoding="utf-8"
            )
            temp_conf = {"pipe_path": pipeline_path.name}
            conf_path = temp_root / "pipeline.conf"
            conf_path.write_text(json.dumps(temp_conf, indent=2), encoding="utf-8")
            configuration = self.resolve_run_conf(conf_path, profile)
            output_dir = temp_root / "results"
            args = self.demo_command(profile, conf_path.relative_to(PROJECT_ROOT), output_dir, configuration)
            timeout = 300 if profile.get("suite", "smoke") == "smoke" else 1800
            with self.job_lock:
                job = self.jobs[job_id]
                job["configuration"] = configuration
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
            if temporary is not None:
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
                payload = service.catalog(query.get("io_binding", [""])[0])
            elif method == "GET" and path == "/api/v1/assets":
                payload = service.assets()
            elif method == "POST" and path == "/api/v1/selection":
                payload = service.verify_selection(body.get("pipeline"), body.get("variant", ""), body.get("model_root", "models"))
            elif method == "GET" and path == "/api/v1/profiles":
                payload = service.profiles()
            elif method == "GET" and path == "/api/v1/pipelines":
                payload = service.pipelines()
            elif method == "GET" and path == "/api/v1/pipeline":
                payload = service.open_pipeline(query.get("filename", [""])[0])
            elif method == "GET" and path == "/api/v1/initial":
                payload = service.initial_pipeline()
            elif method == "GET" and path.startswith("/api/v1/runs/"):
                payload = service.run_status(path.rsplit("/", 1)[-1])
            elif method == "POST" and path == "/api/v1/validate":
                payload = service.validate(
                    body.get("pipeline"), body.get("explain", False)
                )
            elif method == "POST" and path == "/api/v1/fixes/preview":
                payload = service.preview_fix(
                    body.get("pipeline"),
                    body.get("patch", []),
                    body.get("revision") or body.get("expected_revision"),
                    body.get("tool_fingerprint"),
                )
            elif method == "POST" and path == "/api/v1/authoring/preview":
                allowed = {"pipeline", "operation", "operations", "require_valid",
                           "revision", "expected_revision", "tool_fingerprint"}
                unknown = set(body) - allowed
                if unknown:
                    raise StudioError("INVALID_AUTHORING_REQUEST", "未知编排请求字段：" + ", ".join(sorted(unknown)))
                if ("revision" in body and "expected_revision" in body
                        and body["revision"] != body["expected_revision"]):
                    raise StudioError("REVISION_CONFLICT", "请求包含不同的预期修订版本", 409)
                payload = service.preview_authoring(
                    body.get("pipeline"),
                    operation=body.get("operation", _AUTHORING_UNSET),
                    operations=body.get("operations", _AUTHORING_UNSET),
                    require_valid=body.get("require_valid", False),
                    expected_revision=body.get("revision") or body.get("expected_revision"),
                    tool_fingerprint=body.get("tool_fingerprint"),
                )
            elif method == "POST" and path == "/api/v1/preflight":
                payload = service.preflight(
                    body.get("pipeline"),
                    profile_name=body.get("profile", ""),
                    conf_name=body.get("conf_path", "") or body.get("conf_name", ""),
                    model_root=body.get("model_root", "models"),
                    filename=body.get("filename", ""),
                )
            elif method == "POST" and path == "/api/v1/deployment/associate":
                payload = service.associate_deployment(
                    body.get("pipeline_name", "") or body.get("filename", ""),
                    body.get("conf_name", "") or body.get("conf_path", ""),
                    model_root=body.get("model_root", "models"),
                    profile_name=body.get("profile", ""),
                )
            elif method == "POST" and path == "/api/v1/init":

                payload = service.init_pipeline(
                    body.get("io_binding", ""), body.get("profile", ""), body.get("empty", False)
                )
            elif method == "POST" and path == "/api/v1/pipelines":
                payload = service.save_pipeline(
                    body.get("filename", ""), body.get("pipeline"), None, save_as=True
                )
            elif method == "POST" and path == "/api/v1/solutions":
                payload = service.save_solution(
                    body.get("filename", ""), body.get("pipeline"), body.get("profile", ""), body.get("model_root", "models")
                )
            elif method == "PUT" and path == "/api/v1/pipeline":
                payload = service.save_pipeline(
                    body.get("filename", ""),
                    body.get("pipeline"),
                    body.get("revision"),
                    save_as=False,
                    profile_name=body.get("profile", ""),
                    model_root=body.get("model_root", "models"),
                )
            elif method == "POST" and path == "/api/v1/runs":
                payload = service.start_run(
                    body.get("pipeline"), body.get("profile", ""), body.get("model_root", "models"),
                    filename=body.get("filename", ""), conf_name=body.get("conf_name", ""),
                )
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
    binding = SELECTION.pipeline_binding(pipeline)
    print(f"I/O binding: {binding}")
    nodes = pipeline.get("pipeline", [])
    for index, node in enumerate(nodes):
        node_id = node.get("id", "<missing-id>")
        depends = node.get("depends_on", [])
        print(f"  [{index}] {node_id}: {node.get('node_type', 'unknown')}")
        for direction in ("inputs", "outputs"):
            mapping = json.dumps(node[direction], ensure_ascii=False) if direction in node else "<未声明>"
            print(f"      {direction}: {mapping}")
        print(f"      depends_on: {json.dumps(depends, ensure_ascii=False)} (额外顺序)")
    print()


def read_pipeline_file(path: Path) -> dict[str, Any]:
    if path.suffix.lower() != ".json" or not path.is_file():
        raise StudioError("INVALID_PIPELINE_FILE", "请选择一个 Pipeline JSON 文件")
    try:
        with path.open("rb") as stream:
            raw = stream.read(MAX_DOCUMENT_BYTES + 1)
        if len(raw) > MAX_DOCUMENT_BYTES:
            raise StudioError("DOCUMENT_TOO_LARGE", "方案文件超过 4 MiB")
        pipeline = json.loads(raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise StudioError("INVALID_JSON", f"无法解析 JSON：{error}") from error
    except OSError as error:
        raise StudioError("PIPELINE_READ_FAILED", f"无法读取方案文件：{error}") from error
    if not isinstance(pipeline, dict) or not isinstance(pipeline.get("pipeline"), list):
        raise StudioError("INVALID_PIPELINE_DOCUMENT", "方案必须是包含 pipeline 数组的 JSON 对象")
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
