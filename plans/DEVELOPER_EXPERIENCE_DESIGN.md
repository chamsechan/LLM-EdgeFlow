# 业务开发者体验改进：详细设计

| 项 | 内容 |
| --- | --- |
| 状态 | 实施中；按 RFC 的已确认范围执行 |
| RFC | [DEVELOPER_EXPERIENCE_RFC.md](DEVELOPER_EXPERIENCE_RFC.md) |
| 基线 | `main@26d60f2`（2026-10-02） |

本文按 RFC 的编号（WI-1 至 WI-8、O-1、O-2）给出实施细节。文中行号以基线为准，实施时按内容定位。
每项都包含：现状、改动、测试、文档、外部契约影响、验收、回退。WI-8 的三个小项分别并入
WI-2、WI-3、WI-4 实施。

## 0. 通用步骤

### 0.1 采集与比较基线

每个实施分支在改动前、改动后各采集一次，再做比较。采集和比较都必须能识别两种假通过：

- **用旧程序采集**：采集脚本先重新构建 `alg_demo` 和 `alg_pipeline_tool`；构建或任何一条命令失败，
  都立即中止。
- **空跑**：`alg_demo --suite smoke` 在找不到 Profile 时会打印警告并返回 0
  （`demo/main.cpp:71-75`），单条样本失败也不会改变返回码。因此比较脚本要以 catalog 中
  `suite` 为 `smoke` 的 Profile 为准，逐个检查结果文件是否齐全、样本数是否一致；缺任何一项都判失败，
  不能当作"两边一样"。

两个脚本放在 `build/dx-baseline/`（`build/` 已被 Git 忽略）。

`build/dx-baseline/capture.sh`：

```bash
#!/usr/bin/env bash
# Usage: build/dx-baseline/capture.sh build/<capture-dir>
# Rebuilds the tools first so a capture never uses stale binaries.
set -euo pipefail
out="$1"
case "${out}" in
  build/*) ;;
  *) echo "capture directory must be under build/" >&2; exit 2 ;;
esac
rm -rf "${out}"
mkdir -p "${out}"
cmake --build build --target alg_demo alg_pipeline_tool -j 4
./build/alg_pipeline_tool catalog > "${out}/catalog.json"
./build/alg_demo --suite smoke --output-dir "${out}/smoke" > "${out}/smoke.log" 2>&1
{
  echo "commit=$(git rev-parse HEAD)"
  echo "changed_files:"
  git status --short
  grep -E '^(ENABLE_[A-Z_]+|CMAKE_BUILD_TYPE):' build/CMakeCache.txt | sort
  sha256sum build/alg_demo build/alg_pipeline_tool
} > "${out}/manifest.txt"
```

`build/dx-baseline/compare.py`：

```python
#!/usr/bin/env python3
"""Compare two captures; fail unless both are complete and equivalent."""
import json
import sys
from pathlib import Path

ID_KEYS = ("node_type", "model_type", "backend_type", "converter_id",
           "binding_id", "name", "biz_name")
ROW_TIMING_KEYS = {"latency_ms"}  # 发现其他耗时字段时加入
COUNT_KEYS = ("total_samples", "success_count", "failed_count")


def fail(message):
    print(f"FAIL: {message}")
    sys.exit(1)


def load_catalog(capture):
    doc = json.loads((capture / "catalog.json").read_text(encoding="utf-8"))
    if doc.get("ok") is not True or not doc.get("nodes") or not doc.get("profiles"):
        fail(f"{capture}: catalog is not ok or lacks nodes/profiles")
    for key, value in doc.items():
        if isinstance(value, list) and value and isinstance(value[0], dict):
            id_key = next((k for k in ID_KEYS if k in value[0]), None)
            if id_key is None:
                fail(f"{capture}: catalog list '{key}' has no ID field")
            doc[key] = sorted(value, key=lambda item: item[id_key])
    return doc


def load_smoke(capture, expected):
    runs = {}
    for name in expected:
        directory = capture / "smoke" / name
        results = directory / "results.jsonl"
        summary = directory / "summary.json"
        if not results.is_file() or not summary.is_file():
            fail(f"{capture}: profile '{name}' lacks results.jsonl or summary.json")
        rows = [json.loads(line) for line in
                results.read_text(encoding="utf-8").splitlines() if line.strip()]
        meta = json.loads(summary.read_text(encoding="utf-8"))
        total = meta.get("total_samples", 0)
        if (meta.get("profile") != name or total <= 0 or len(rows) != total or
                meta.get("success_count", -1) + meta.get("failed_count", -1) != total):
            fail(f"{capture}: profile '{name}' results and summary are inconsistent")
        runs[name] = {
            "counts": {key: meta[key] for key in COUNT_KEYS},
            "rows": [{k: v for k, v in row.items() if k not in ROW_TIMING_KEYS}
                     for row in rows],
        }
    extra = sorted(p.name for p in (capture / "smoke").iterdir()
                   if p.is_dir() and p.name not in expected)
    if extra:
        fail(f"{capture}: unexpected smoke result directories {extra}")
    return runs


def build_options(capture):
    lines = (capture / "manifest.txt").read_text(encoding="utf-8").splitlines()
    return sorted(line for line in lines
                  if line.startswith(("ENABLE_", "CMAKE_BUILD_TYPE")))


before, after = (Path(arg) for arg in sys.argv[1:3])
if build_options(before) != build_options(after):
    fail("captures were built with different CMake options")
catalog_before, catalog_after = load_catalog(before), load_catalog(after)
if catalog_before != catalog_after:
    fail("catalog differs")
expected = sorted(p["name"] for p in catalog_before["profiles"]
                  if p.get("suite") == "smoke")
if not expected:
    fail("baseline catalog lists no smoke profiles")
if load_smoke(before, expected) != load_smoke(after, expected):
    fail("smoke results differ")
print(f"identical ({len(expected)} smoke profiles)")
```

使用方法：

```bash
build/dx-baseline/capture.sh build/dx-before   # 改动前
# ……实施改动……
build/dx-baseline/capture.sh build/dx-after    # 改动后
python3 build/dx-baseline/compare.py build/dx-before build/dx-after
```

第一次使用前先自检，确认空结果会被判为失败：

```bash
mkdir -p build/dx-empty/smoke
cp build/dx-before/catalog.json build/dx-before/manifest.txt build/dx-empty/
python3 build/dx-baseline/compare.py build/dx-before build/dx-empty   # 必须输出 FAIL 并返回 1
```

比较通过的标准是：返回 0，并输出 `identical (<N> smoke profiles)`，N 等于 catalog 中 smoke Profile 的数量
（基线为 9）。`manifest.txt` 记录提交号、改动文件、构建选项和两个程序的 SHA-256，附在 PR 中备查。

### 0.2 每个 PR 的通用要求

- 从最新 `main` 建分支（RFC 第 7 节列出了分支名）。
- PR 描述写明问题、方案、受影响的契约、验收结果；附上 0.1 的比较输出。
- 本地交接运行 `./scripts/run_all_tests.sh`；远端交付只在明确授权后使用
  `scripts/git_branch_upload.sh`。
- 用户能感知到的变化写进 `doc/CHANGELOG.md` 的 Unreleased。

---

## WI-1 业务源码自动收录

### 1.1 现状

| 位置 | 内容 |
| --- | --- |
| `src/custom_nodes/CMakeLists.txt:3-5` | 显式列出 `prompt_guided_llm_node.cpp` |
| `src/common_nodes/CMakeLists.txt:1-13` | 显式列出 12 个源文件（含 `support/compiled_text_regex.cpp`） |
| `src/adapter/CMakeLists.txt:11-33` | 显式列出 `input/`、`output/`、`biz/` 下 23 个源文件，与框架机制文件混在同一个列表 |
| `demo/CMakeLists.txt:6-12` | 显式列出 `biz/` 下 7 个源文件 |
| `tools/scaffold_custom_node.py` | `updated_cmakelists`（770-777）、`--add-to-cmake` 参数（939）、`cmake_path`（970）、修改 CMakeLists（982-985）、dry-run 输出（995-997）、提示输出（1006-1017） |
| `tools/dev_recipe.py` | 产物说明（51）、修改 CMakeLists（306-308） |
| `tests/tooling/generate_scaffold_fixtures.py:52-59` | 复制 CMakeLists 并传 `--add-to-cmake` |
| `tests/ScaffoldFixtures.cmake:32` | 依赖 `src/custom_nodes/CMakeLists.txt` |

测试目录已使用自动收录：`tests/RuntimeTests.cmake:41`。

`scripts/check_layer_isolation.sh:316-330` 只检查各层 CMakeLists 中出现
`target_sources(<层目标>` 文本，因此改用变量后仍然通过。

### 1.2 CMake 改动

收录规则：目录下所有 `.cpp`（包括子目录）都编入。选 `GLOB_RECURSE` 是为了让放进子目录的
文件也被编入，不再出现"文件在、却没编译"的情况。

`src/custom_nodes/CMakeLists.txt` 全文替换为：

```cmake
# Business-owned Node sources are collected automatically: every .cpp under this
# directory, including subdirectories, is compiled into the Capability Nodes
# layer. CONFIGURE_DEPENDS re-globs on the next build after files change.
file(GLOB_RECURSE EDGEFLOW_CUSTOM_NODE_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
target_sources(edgeflow_capability_nodes_objects PRIVATE
  ${EDGEFLOW_CUSTOM_NODE_SOURCES})
```

`src/common_nodes/CMakeLists.txt` 同样替换，变量名为 `EDGEFLOW_COMMON_NODE_SOURCES`。

`src/adapter/CMakeLists.txt`：框架机制文件保持显式；删除第 11-33 行，改为引用变量：

```cmake
# Framework mechanism files stay explicit. Business converters and bindings
# under input/, output/ and biz/ are collected automatically.
file(GLOB_RECURSE EDGEFLOW_ADAPTER_BIZ_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/input/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/output/*.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/biz/*.cpp")
target_sources(edgeflow_integration_objects PRIVATE
  deployment_model_resolver.cpp
  # ...其余机制文件保持原样...
  io_catalog.cpp
  ${EDGEFLOW_ADAPTER_BIZ_SOURCES}
  operator/json_output_config_reader.cpp
  # ...operator/ 下文件保持原样...
  operator/operator_builtin_value_types.cpp)
```

`demo/CMakeLists.txt`：

```cmake
file(GLOB_RECURSE EDGEFLOW_DEMO_BIZ_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/biz/*.cpp")
add_library(edgeflow_demo_objects OBJECT
  common/demo_options.cpp
  common/demo_registry.cpp
  common/dataset_reader.cpp
  common/result_writer.cpp
  ${EDGEFLOW_DEMO_BIZ_SOURCES})
```

`src/core`、`src/engine` 不改：Backend 按构建选项条件编译，需要保持显式。

为什么源文件顺序变化不影响行为：

- 节点、转换器、绑定注册表都用 `unordered_map` 存储，节点和业务列表输出前会排序
  （`src/core/node_registry.cpp:254`、`src/core/pipeline_catalog.cpp:126`）；
- `IoBindingRegistry::RegisterBinding`（`src/adapter/io_binding_registry.cpp:124`）
  只检查自身字段，与转换器的交叉校验在 Init 审计时进行，不依赖静态初始化顺序。

### 1.3 工具改动

`tools/scaffold_custom_node.py`：

1. 删除 `updated_cmakelists`、`--add-to-cmake` 参数、`cmake_path` 变量，以及对应的修改、
   dry-run 输出和提示分支。
2. 写入成功后的提示改为：

   ```text
   Created <source>
   Sources under src/custom_nodes/ are compiled automatically on the next build.
   ```

   使用 `--write-test` 时，继续输出原有的构建命令和测试过滤器提示（原 1008-1012 行）。
3. 保留 `ChangePlan`：它负责源文件和测试文件的原子写入，以及 `--force` 覆盖。
4. 项目不保留旧参数别名。传入 `--add-to-cmake` 时由 argparse 报 "unrecognized arguments"。

`tools/dev_recipe.py`：

1. 删除 306-308 行对 CMakeLists 的修改。
2. 第 51 行产物说明改为 `"Node source, unit test, Pipeline JSON, .conf, effects sample."`。

`tests/tooling/generate_scaffold_fixtures.py`：

1. 删除 52-55 行复制 CMakeLists 的代码，改为创建目录：
   `(fixture_root / "src/custom_nodes").mkdir(parents=True, exist_ok=True)`。
2. 第 59 行去掉 `"--add-to-cmake"`。

`tests/ScaffoldFixtures.cmake:32`：从 DEPENDS 中删除 `src/custom_nodes/CMakeLists.txt`。

### 1.4 测试改动

`tests/tooling/test_scaffold_custom_node.py`：

| 测试 | 处理 |
| --- | --- |
| `_setup_mock_repo`（24-32） | 不再复制 CMakeLists，只建目录 |
| `test_file_registration_overwrite_and_dry_run`（60-77） | 去掉 `--add-to-cmake`；保留 dry-run 不写文件、拒绝覆盖用户修改、`--force` 覆盖三项断言；删除关于 CMakeLists 内容的断言；改名为 `test_overwrite_and_dry_run` |
| `test_bad_cmake_does_not_leave_partial_source`（79-83） | 删除：不再修改 CMakeLists，这个失败路径不存在 |
| `test_write_test_creates_source_and_test_file`（148-180） | 删除 "Pending registrations" 和 "Add ... to CMakeLists" 断言，改为断言输出 "compiled automatically"，且目录中没有新建或修改任何 CMakeLists |
| `test_write_test_add_to_cmake_needs_only_production_registration`（181-215） | 删除，覆盖内容已并入上一项 |
| `test_write_test_dry_run_does_not_create_files`（216-244） | 参数去掉 `--add-to-cmake`，删除 CMakeLists 断言 |
| `test_write_test_atomic_rollback_on_conflict`（270-329） | Case 1、2 去掉 `--add-to-cmake`；删除 Case 3（损坏的 CMakeLists） |
| `test_preexisting_temporary_file_and_user_edited_rollback_survive`、`test_failed_second_registration_preserves_concurrent_first_registration` | 保留：它们测试 `ChangePlan` 的通用语义，文件名只是样例 |
| `test_generates_function_nodes_and_rejects_retired_options`（436-） | 第 461 行去掉 `--add-to-cmake`；在 retired 参数列表中加入 `["--add-to-cmake"]` |

`tests/tooling/test_dev_recipe.py`：

- 第 41 行：从复制列表中删除 `src/custom_nodes/CMakeLists.txt`，改为创建 `src/custom_nodes/` 目录。
- 第 164 行：断言改为 `self.assertFalse((self.root / "src/custom_nodes/CMakeLists.txt").exists())`。

### 1.5 文档与 Skill

| 文件 | 行 | 改动 |
| --- | --- | --- |
| `doc/dev_guide/first_custom_node.md` | 10 | "测试、CMake 登记和可运行方案" → "测试和可运行方案" |
| 同上 | 22 | 删除表格行 "`src/custom_nodes/CMakeLists.txt` \| 脚手架自动登记源码" |
| 同上 | 50 | 命令去掉 `--add-to-cmake` |
| 同上 | 108 | 改为"新文件在下次构建时自动编入，重新编译后才会进入 Catalog。" |
| 同上 | 175-176 | 删除"`--add-to-cmake` 登记生产 Node 源码。" |
| `doc/dev_guide/first_control.md` | 11 | 删除表格行"新节点登记编译" |
| 同上 | 28、40 | 命令和说明去掉 `--add-to-cmake` |
| 同上 | 65 | "只在你生成并登记源码后" → "只在你生成源码并重新构建后" |
| `doc/dev_guide/custom_node_concepts.md` | 263 | "文件是否登记进 CMake" → "文件是否位于 `src/custom_nodes/`" |
| `doc/dev_guide/recipe_text_llm_node.md` | 3 | 删除"生产源码的 CMake 登记" |
| `doc/dev_guide/business_onboarding.md` | 91 | "构建与部署"一行删除两个 CMake 链接，保留 Pipeline 和部署配置 |
| 同上 | 137-138 | 删除步骤 4"登记构建" |
| 同上 | 227-228 | 删除"将新增源码加入 `demo/CMakeLists.txt`" |
| 同上 | 295 | "完成源码登记后重新构建" → "重新构建" |
| `src/custom_nodes/README.md` | 21-24 | 4 条命令去掉 `--add-to-cmake` |
| 同上 | 30-31 | 删除"未用 `--add-to-cmake` 时，将源码加入本目录 CMakeLists.txt"；补一句"目录下所有 `.cpp`（含子目录）都会编入，草稿不要用 `.cpp` 后缀" |
| `tests/README.md` | 59-60 | 删除两句 `--add-to-cmake` 说明（这一节随后由 WI-3 整体迁移） |
| `.agents/skills/edgeflow-node-map-developer/SKILL.md` | 23、30-31 | 命令去掉参数；"登记到 `src/common_nodes/CMakeLists.txt`" → "放在 `src/common_nodes/`" |
| `.agents/skills/edgeflow-node-llm-developer/SKILL.md` | 23、26-27 | 命令去掉参数；"并登记源码及测试" → "并生成测试"；"登记到 common 的 CMake" → "放在 `src/common_nodes/`" |
| `.agents/skills/edgeflow-node-batch-developer/SKILL.md` | 53-54、58 | "源码加入所属 common/custom CMake 的 `edgeflow_capability_nodes_objects`" → "源码放在 `src/common_nodes/` 或 `src/custom_nodes/`，自动编入"；删除 `--add-to-cmake` 说明 |
| `.agents/skills/edgeflow-adapter-developer/SKILL.md` | 27 | 删除"编译"一行 |

`doc/CHANGELOG.md`（Unreleased）增加：

> 业务源码自动收录：`src/custom_nodes/`、`src/common_nodes/`、`src/adapter/{input,output,biz}/`
> 与 `demo/biz/` 下的 `.cpp` 在下次构建时自动编入，不再需要修改 CMake。脚手架删除
> `--add-to-cmake`，recipe 不再修改 CMakeLists。

### 1.6 外部契约影响

无。只改构建脚本和开发工具；SDK 导出符号、Catalog 内容不变（见验收）。

### 1.7 验收

- [ ] 0.1 的比较通过：用重新构建后的程序采集，输出 `identical (9 smoke profiles)`，返回 0。
- [ ] 用脚手架在 `src/custom_nodes/` 生成一个临时节点，不改 CMake，重新构建后
      `describe-node` 能查到；删除文件并重新构建后查不到。在临时子目录中放一个 `.cpp`，重复同样的验证。
      过程记录在 PR 中，临时文件不提交。
- [ ] 在 `demo/biz/` 和 `src/adapter/biz/` 各做一次同样的增删验证（可用复制现有文件、改注册名的方式）。
- [ ] `git grep -n -- '--add-to-cmake'` 只命中 CHANGELOG 中的删除说明，以及 `plans/` 下的计划文档。
- [ ] `LayerGuardTest`、`LayerGuardSelfTest`、`CustomNodeScaffoldTest`、Recipe 相关测试通过；
      统一门禁通过。

### 1.8 回退与备选方案

回退：还原本 PR。

备选方案（内网构建系统不接受 GLOB 时使用）：恢复显式列表，并新增检查脚本
`scripts/check_source_registration.py`。脚本列出四个业务目录中的全部 `.cpp`，与各自 CMakeLists
中出现的文件名比较，有遗漏就报错，并给出应加入的文件和 CMakeLists 路径。该检查纳入
`LayerGuardTest` 同一标签组。这样能把静默失败变成明确报错，但仍需手工登记。

---

## WI-2 注册类诊断（含 WI-8b）

### 2.1 现状

在基线上用生产版工具校验 mock 配置：

```bash
./build/alg_pipeline_tool validate demo/fixtures/mock/pipeline_entity_extract_custom.json
```

得到 3 条诊断（摘录）：

| code | path | 说明 |
| --- | --- | --- |
| `UNKNOWN_MODEL_TYPE` | `/models/0/model_type` | 只有 `Unknown model_type: test_biz_llm`，没有 remediation |
| `UNKNOWN_BACKEND` | `/models/0/backend` | 只有 `Unknown backend: test_causal_lm_backend`，没有 remediation |
| `UNKNOWN_MODEL_REFERENCE` | `/pipeline/0/config/bind_model` | 连带误报：summary 说模型 `entity_llm`"不符或未声明"，而该模型已在 `models` 中声明 |

把关键词配置中的 `TextRuleMatchNode` 改成 `TextRuleMatchNod` 再校验，也得到 3 条：
`UNKNOWN_NODE_TYPE`（没有 remediation），以及 path 分别为 `/pipeline` 和 `/io/output` 的两条
`MISSING_BIZ_OUTPUT`。后两条是同一根因的连带结果，而且互相重复。

相关源码：

| 位置 | 内容 |
| --- | --- |
| `include/core/remediation_cause.h:5-16` | remediation cause 列表 |
| `src/core/pipeline_validator.cpp:117-163` | 配置字段校验；136-140 行把全部字段按 schema 顺序放进 `suggestions` |
| `src/core/pipeline_validator.cpp:265-284` | `LevenshteinDistance` |
| `src/core/pipeline_validator.cpp:344-` | `PopulateBasicRemediation`；372-386 行已按编辑距离生成 `candidate_fields` |
| `src/core/pipeline_validator.cpp:907-924` | 模型与 Backend 是否注册的检查 |
| `src/core/pipeline_validator.cpp:1037-1045` | 节点类型是否注册的检查 |
| `src/core/pipeline_validator.cpp:1086-1103` | 模型引用检查 |
| `src/core/pipeline_validator.cpp:1411-1420`、`1450-1474` | 业务出口、IO 边界的"缺少输出"检查 |
| `src/cli/alg_pipeline_tool.cpp:475-540` | `validate` / `plan` 子命令 |
| `tests/CMakeLists.txt:27-36` | `alg_pipeline_tool_test`：同一份 CLI 源码，额外链接测试替身 |
| `tools/pipeline_studio/web/editor.js:48-50` | Studio 把 `remediation.summary` 显示为"原因诊断"，并显示 `suggestions` |
| `src/adapter/io_binding_resolver.cpp:198-210` | SDK 报错只取第一条诊断的 code、path、message |

### 2.2 Core：增加 remediation

**新增 cause**（`include/core/remediation_cause.h`，追加到列表末尾）：

```cpp
  X(kUnknownNodeType, "unknown_node_type")                  \
  X(kUnknownModelType, "unknown_model_type")                \
  X(kUnknownBackend, "unknown_backend")
```

**公共辅助函数**（`src/core/pipeline_validator.cpp` 匿名命名空间）。把 `LevenshteinDistance`
移到 `ValidateAndNormalizeConfig` 之前，并新增：

```cpp
// Orders names by edit distance to `target`; ties are ordered by name.
std::vector<std::string> RankByEditDistance(std::string_view target,
                                            std::vector<std::string> names);

// Up to `limit` names whose distance is at most max(2, target.size() / 3).
std::vector<std::string> NearestNames(std::string_view target,
                                      std::vector<std::string> names,
                                      size_t limit = 3);
```

`PopulateBasicRemediation` 中已有的 `candidate_fields` 排序（372-386 行）改为调用
`RankByEditDistance`，行为不变。

**WI-8b**：`ValidateAndNormalizeConfig` 的 136-140 行改为
`diag.suggestions = RankByEditDistance(err.field_name, <schema 字段名>)`。这样顶层 `suggestions`
与 `candidate_fields` 的顺序一致。Studio 显示的"修复建议"用的正是顶层 `suggestions`。

**`PopulateBasicRemediation` 新增三个分支**：

| code | 定位 | facts | summary |
| --- | --- | --- | --- |
| `UNKNOWN_NODE_TYPE` | `/pipeline/<i>/node_type` | `node_type`、`candidate_node_types` | "节点 '<id>' 的 node_type '<X>' 未在当前构建中注册。[相近的已注册类型：A、B。]新增的 Node 需要重新构建后才会注册。" |
| `UNKNOWN_MODEL_TYPE` | `/models/<i>/model_type` | `model_id`、`model_type`、`candidate_model_types`、`registered_model_types` | "模型 '<model_id>' 的 model_type '<X>' 未在当前构建中注册。[相近的已注册类型：A、B。]" |
| `UNKNOWN_BACKEND` | `/models/<i>/backend` | `model_id`、`backend`、`candidate_backends`、`registered_backends` | "模型 '<model_id>' 的 backend '<X>' 未在当前构建中注册。[相近的已注册 Backend：A。]可选 Backend 需要在构建时启用。" |

- `candidate_*` 由 `NearestNames` 计算，同时写入该诊断的顶层 `suggestions`。
- 节点候选来自 `catalog.nodes`；模型和 Backend 来自 `PipelineCatalog::Models()` / `Backends()`，
  `registered_*` 按名称排序后写入。
- 文案不提具体工具名：Core 校验也在 SDK 的 Create 路径中运行。
- `message` 和 `code` 保持不变。

### 2.3 Core：去掉连带诊断

在 `ValidateAndPlanInternal` 中增加三个局部集合：

```cpp
std::unordered_set<std::string> unresolved_model_ids;   // model_type 未注册的模型
std::unordered_set<std::string> unresolved_output_keys; // 未注册节点显式映射的输出键
std::unordered_set<std::string> reported_missing_outputs;
```

原则：只抑制**完全由未注册项造成**的诊断。同一位置上只要还存在可以独立判断的错误
（已知生产者类型不符、生产者不唯一、与 ingress 冲突、能力不符），就照常报告。

| 规则 | 位置 | 抑制条件（必须全部满足） |
| --- | --- | --- |
| R1：model_type 未注册时，不再报引用它的 `UNKNOWN_MODEL_REFERENCE` | 907-924 行记录 `unresolved_model_ids`；只改 1096-1098 行"引用解析失败"这一分支 | ① 引用解析失败（`model_capabilities` 中找不到该 id）；② 该 id 在 `models` 中已声明，但 model_type 未注册。能力不符分支（1099-1103 行）不受影响；引用真正未声明的 id 照常报告。重复的 model_id 在解析阶段已被拒绝（`src/core/pipeline_config.cpp:182`），不会与本规则冲突 |
| R2：节点类型未注册时，只抑制由它造成的"缺少生产者" | 1037-1045 行把该节点 `ports.outputs` 的值加入 `unresolved_output_keys` | ① 键属于 `unresolved_output_keys`；② `producers` 中没有该键的任何已知生产者；③ 该键不在 ingress 中。适用于三处：业务出口（1411-1420 行，该分支本身已满足 ②）、IO 边界（1466-1472 行，本身已满足 ②③）、下游节点输入（1314 行）。下游节点输入处现有的 `!found`（1285-1316 行）同时代表"没有生产者""已知生产者类型不符""生产者不唯一""与 ingress 冲突"四种情况，实现时必须先按 ②③ 区分出"完全没有已知来源"，只跳过这一种 |
| R3：业务出口与 IO 边界对同一个缺失键只报一次 | 1411-1420 行报告后记入 `reported_missing_outputs`；1466-1472 行遇到已报告的键时跳过 | 两处报告的是同一种情况：没有已知生产者，且（IO 边界一侧）不在 ingress 中。保留 path 为 `/pipeline` 的那一条 |

基线上的实测（复现配置见附录 A）说明了 R2 为什么必须带条件 ②③：

| 场景 | 当前输出 | R2 不带条件时 | R2 带条件时 |
| --- | --- | --- | --- |
| `combo_type`：拼错的节点和一个已知节点都映射到 `shared_key`，已知节点输出 `RuleMatchBatch`，下游节点需要 `TextBatch` | `UNKNOWN_NODE_TYPE` + 下游的 `MISSING_INPUT_PRODUCER` | 下游诊断被吞掉，类型不符这个独立错误消失 | 两条都保留 |
| `combo_cascade`：拼错的节点是下游所读键的唯一生产者 | `UNKNOWN_NODE_TYPE` + 下游的 `MISSING_INPUT_PRODUCER` | 只剩 `UNKNOWN_NODE_TYPE` | 只剩 `UNKNOWN_NODE_TYPE` |

根因诊断总是先于被抑制的诊断产生（模型检查在节点检查之前，节点类型检查在输出检查之前），
所以诊断列表的第一条不变。SDK 报错只取第一条（`io_binding_resolver.cpp:198-210`），宿主看到的
错误文本不变。

`tests/unit/core/test_validated_pipeline_plan.cpp:806-820` 只设置了 IO 边界、没有业务定义，
R3 不影响它。

### 2.4 CLI：工具层提示

`src/cli/alg_pipeline_tool.cpp` 新增：

```cpp
// Production tool only. Explains unknown registrations caused by build
// variants or test doubles; stdout JSON stays unchanged.
void PrintRegistrationHint(const nlohmann::json& response) {
#ifndef LLM_EDGEFLOW_TOOL_HAS_TEST_REGISTRATIONS
  // response["diagnostics"] 中出现 UNKNOWN_MODEL_TYPE 或 UNKNOWN_BACKEND 时，
  // 向 std::cerr 输出下面的提示，每次运行最多一次。
#endif
}
```

提示文案：

```text
提示：当前 alg_pipeline_tool 只包含本次构建启用的生产注册。
  - 可选 Backend 需要用对应的构建预设重新构建，见 doc/VERIFIABLE_SELECTION.md#构建变体。
  - 使用测试 Model/Backend 的配置（例如 demo/fixtures/mock/ 下的方案）请改用
    alg_pipeline_tool_test，见 tools/pipeline_studio/README.md#校验工具选择。
```

- 调用点：`validate`、`plan`（含 `--explain`）、`validate-io`、`resolve-conf` 在打印 JSON 之后调用；
  `describe-model`、`describe-backend` 找不到类型时也调用。
- `tests/CMakeLists.txt` 为 `alg_pipeline_tool_test` 增加
  `target_compile_definitions(alg_pipeline_tool_test PRIVATE LLM_EDGEFLOW_TOOL_HAS_TEST_REGISTRATIONS=1)`。
- 提示只写 stderr；Studio（`tools/pipeline_studio/server.py`）和自动化只解析 stdout，不受影响。

### 2.5 可选：`--explain` 名称修正建议

如果实现成本低，同一个 PR 中在 `PipelineValidator::Explain` 增加名称修正：对上述三个 cause，
取编辑距离不超过 2 的候选，生成 `replace` 补丁，再由现有的验证机制确认。Studio 的
"预览并应用"会自动支持。成本较高时拆到后续 PR，不影响本项验收。

### 2.6 改动后的输出（预期）

mock 配置经生产工具校验，stdout 中只剩 2 条诊断：

```json
{
  "code": "UNKNOWN_MODEL_TYPE",
  "path": "/models/0/model_type",
  "message": "Unknown model_type: test_biz_llm",
  "remediation": {
    "cause": "unknown_model_type",
    "facts": {
      "model_id": "entity_llm",
      "model_type": "test_biz_llm",
      "candidate_model_types": [],
      "registered_model_types": ["...按名称排序..."]
    },
    "summary": "模型 'entity_llm' 的 model_type 'test_biz_llm' 未在当前构建中注册。"
  }
}
```

第二条 `UNKNOWN_BACKEND` 结构相同。stderr 输出 2.4 的提示。`TextRuleMatchNod` 的例子只剩
1 条 `UNKNOWN_NODE_TYPE`，其 `suggestions` 为 `["TextRuleMatchNode"]`。

### 2.7 测试

`tests/integration/pipeline/test_pipeline_catalog_validator.cpp`（`PipelineValidatorTest`，
在 `edgeflow_test_tooling_runner` 中运行，已注册测试替身）新增：

| 测试 | 断言 |
| --- | --- |
| `UnknownModelTypeHasRemediationWithoutReferenceCascade` | 把 mock 配置的 model_type 改为 `test_biz_llmm`：有 `UNKNOWN_MODEL_TYPE`，cause 为 `unknown_model_type`，`suggestions[0] == "test_biz_llm"`；没有 `UNKNOWN_MODEL_REFERENCE` |
| `UndeclaredModelReferenceIsStillReported` | `bind_model` 改为未声明的 id：仍有 `UNKNOWN_MODEL_REFERENCE` |
| `UnknownBackendHasRemediation` | backend 改为 `test_causal_lm_backnd`：cause 为 `unknown_backend`，`suggestions[0] == "test_causal_lm_backend"`，`registered_backends` 非空 |
| `UnknownNodeTypeSuppressesMissingOutputCascade` | 关键词配置的节点类型改为 `TextRuleMatchNod`，带业务定义和 IO 边界：只有 `UNKNOWN_NODE_TYPE`，`suggestions[0] == "TextRuleMatchNode"` |
| `MissingBizOutputIsReportedOnce` | 删除产出业务出口键的节点，带业务定义和 IO 边界：该键的 `MISSING_BIZ_OUTPUT` 恰好一条，path 为 `/pipeline` |
| `UnknownNodeSoleProducerSuppressesConsumerCascade` | `combo_cascade` 场景：只有 `UNKNOWN_NODE_TYPE`，下游没有 `MISSING_INPUT_PRODUCER` |
| `UnknownNodeDoesNotHideKnownProducerTypeMismatch`（组合错误） | `combo_type` 场景：`UNKNOWN_NODE_TYPE` 与下游 `/pipeline/3/inputs/text` 的 `MISSING_INPUT_PRODUCER` 同时存在 |
| `UnknownNodeDoesNotHideDuplicateProducers`（组合错误） | 拼错的节点和两个已知节点映射到同一个键：`DUPLICATE_PORT_PRODUCER` 与下游的 `MISSING_INPUT_PRODUCER` 都保留 |
| `UnknownNodeDoesNotHideIngressTypeMismatch`（组合错误） | 拼错的节点把输出映射到 ingress 键 `input_sentences`，另有一个输入类型不是 `TextBatch` 的已注册节点读取该键：下游诊断保留 |
| `UnknownModelTypeDoesNotHideCapabilityMismatch`（组合错误） | 一个模型的 model_type 拼错，另一个已注册模型的能力与引用它的节点不符：`UNKNOWN_MODEL_TYPE` 与 `MODEL_CAPABILITY_MISMATCH` 都保留 |
| `UnknownConfigFieldSuggestionsAreRanked`（WI-8b） | 配置字段写成 `categoriess`：`suggestions[0] == "categories"`，并且与 `candidate_fields` 顺序一致 |

`tests/tooling/test_pipeline_studio.py` 新增 `test_production_tool_explains_unknown_registrations`：

- 用生产工具（环境变量 `LLM_EDGEFLOW_SELECTION_TOOL`）校验 mock 配置：返回码 1；stdout 可以解析，
  诊断 code 为 `UNKNOWN_MODEL_TYPE`、`UNKNOWN_BACKEND` 两条；stderr 包含 `alg_pipeline_tool_test`。
- 用测试工具（`LLM_EDGEFLOW_PIPELINE_TOOL`）校验同一配置：`ok` 为 true，stderr 不含提示。

### 2.8 文档

- `tools/pipeline_studio/README.md`"校验工具选择"一节补一句："生产工具遇到未注册的模型或
  Backend 时，会在 stderr 提示检查构建变体或改用测试工具。"
- `doc/CHANGELOG.md`（Unreleased）：

  > 校验诊断：未注册的节点类型、模型类型和 Backend 给出原因与相近的已注册名称；同一根因的
  > 连带诊断不再重复报告；未知配置字段的建议按相似度排序。生产版 `alg_pipeline_tool` 遇到未注册的
  > 模型或 Backend 时，在 stderr 提示检查构建变体或改用 `alg_pipeline_tool_test`。

### 2.9 外部契约影响

无。

- Operator 与 SDK 的报错文本不变（理由见 2.3）。
- 校验 JSON 只增加 remediation 字段和 cause 取值，原有字段不变；`schema_version` 保持 1。
- 诊断条目减少属于内部工具行为的变化，见 RFC 3.2。

### 2.10 验收

- [ ] 2.1 的两个场景按 2.6 输出：mock 配置 3 条 → 2 条，并带 remediation；拼错节点类型 3 条 → 1 条。
      在 PR 中贴出前后对比。
- [ ] 组合错误不被隐藏：`combo_type` 改动前后都报告下游的 `MISSING_INPUT_PRODUCER`；
      只有 `combo_cascade` 的下游诊断被抑制。在 PR 中贴出两个场景改动前后的输出。
- [ ] 2.7 的测试通过；`PipelineStudioServerTest`、`PipelineStudioTest`、`OperatorApiTest`、
      `IoBindingRegistryTest` 全部通过，且未修改原有断言。
- [ ] 0.1 的比较通过（标准见 0.1 末尾）。
- [ ] 统一门禁通过。

回退：还原本 PR。

---

## WI-3 业务接入指南按读者拆分（含 WI-8c）

### 3.1 现状

`doc/dev_guide/business_onboarding.md` 共 319 行，章节如下：

| 行 | 章节 | 主要读者 |
| --- | --- | --- |
| 11-29 | 输入输出以 Operator 接口为边界 | 转换器作者（锚点被 6 处引用） |
| 31-60 | 1. 先确定要走哪条路径 | 转换器作者 |
| 62-95 | 2. 用一个现有业务看清文件关系 | 转换器作者 |
| 97-179 | 3. 实现并注册转换器与绑定 | 转换器作者；154-162 行特殊槽位命名的细节与 `operator_output_allocation.md`"选择参数"表重复 |
| 181-206 | 4. Operator 类型与输出池 | 新宿主类型的作者；内容与 `operator_output_allocation.md`"实现与注册"一节重复（type traits、`RegisterOperatorValueType`、`MakeTypedInputBinding`、`MakePooledOutputBinding`、`MakeOutputParameterParser`） |
| 208-236 | 5. 统一 Demo 接入 | 转换器作者（锚点被 Skill 引用） |
| 238-291 | 6. 输出容量与生命周期 | 混合：容量配置写给转换器作者；借用输入、输出租约、销毁顺序、Init/DeInit、错误码写给宿主集成方 |
| 293-319 | 7. 最小验证 | 转换器作者 |

当前没有检查 Markdown 链接和锚点的门禁。

### 3.2 先合入链接检查

新增 `scripts/check_doc_links.py`，作为本 PR 的第一个提交，在移动内容之前合入。

**检查范围**：`git ls-files '*.md'` 列出的文件；不在 Git 仓库中时，退回遍历仓库目录，
排除 `build*/`、`3rdparty/`、`.git/`。

**规则**：

1. 提取 Markdown 行内链接的目标（方括号文字之后紧跟的圆括号内容）；跳过代码围栏内的行和行内代码。
2. 跳过带协议的链接（`http:`、`https:`、`mailto:` 等）。
3. 相对路径以所在文件的目录为基准，目标文件或目录必须存在。
4. 带锚点且目标是 `.md` 文件时（包括同文件的 `#anchor`），锚点必须等于目标文件某个标题的 slug。
   目标不是 `.md` 时（例如源码的 `#L42`），不检查锚点。

**slug 规则**（与 GitHub 一致）：

- 去掉标题中的行内代码标记和链接语法；
- 转为小写；
- 删除 Unicode 标点和符号类字符，保留 `-` 和 `_`；
- 空格替换为 `-`；
- 重复的标题依次加 `-1`、`-2`。

**防空跑**：扫描到的 Markdown 文件为 0，或检查过的跨文件锚点链接为 0 时，直接判失败并打印两个计数。
这样文件枚举出错时，不会因为"什么都没检查"而报告 0 处失效。正常运行时也打印这两个计数，便于在 PR 中核对。

**自测**：`--self-test` 在临时目录中构造正确链接、缺失文件、缺失锚点、重复标题、代码围栏内的链接
五种情况，以及"没有任何 Markdown 文件"的空目录，逐一验证判定结果。

**CTest 注册**（`tests/RuntimeTests.cmake`，放在 `ArchitectureDocsDriftTest` 旁）：

```cmake
add_test(NAME DocLinksTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_doc_links.py)
add_test(NAME DocLinksSelfTest
  COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_doc_links.py --self-test)
set_tests_properties(DocLinksTest DocLinksSelfTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
```

**基线**：用原型脚本扫描，53 个 Markdown 文件、59 处跨文件锚点链接，0 处失效。原型对
`#输入输出以-operator-接口为边界`、`#6-输出容量与生命周期`、`#5-统一-demo-接入`、
`#fast-feedback-for-solution-authors` 等现有中英文锚点的 slug 计算结果都正确。

### 3.3 business_onboarding.md 的新结构

| 章节 | 处理 |
| --- | --- |
| 开头（1-9 行） | 增加一句读者说明："本文写给新增或调整业务契约的转换器作者。新宿主类型、输出池实现和宿主调用规则见《Operator 宿主类型、输出池与生命周期》。"（链接到 `operator_output_allocation.md`） |
| 输入输出以 Operator 接口为边界 | 不变 |
| 1 | 不变 |
| 2 | 表格"Operator 类型注册"一行注明"仅新宿主类型需要"，并链接 `operator_output_allocation.md#实现与注册`；"构建与部署"一行已由 WI-1 修改 |
| 3 | 154-162 行压缩为两句："常见必需槽使用 `ExternalInputSlot<T>(slot)`、`ExternalOutputSlot<T>(slot)`。后缀与槽名不同、可选槽或特殊布局使用完整的 `ExternalSlotDefinition`，字段含义见'选择参数'。"（链接到 `operator_output_allocation.md#选择参数`）163-166 行（测试中的多槽视图与容量说明）移到第 7 节 |
| 4 | 标题改为"4. 需要新的宿主类型时"，正文改为两句：多数业务复用已注册的宿主类型；确需新的平台结构或分配布局时，见"实现与注册"一节（链接到 `operator_output_allocation.md#实现与注册`）。原第 4 节锚点没有入链 |
| 5 | 保留标题，以保持 `#5-统一-demo-接入` 锚点；WI-1 已删除 CMake 步骤 |
| 6 | 标题改为"6. 输出容量"，见 3.4 |
| 7 | 增加 3.7 的本地快速验证，以及从第 3 节移来的测试容量说明 |

### 3.4 移动清单

| 原位置（business_onboarding.md） | 内容 | 去向 |
| --- | --- | --- |
| 183-201 | ValueType、type traits、注册函数、命名分配方案 | 删除，`operator_output_allocation.md`"实现与注册"一节已有 |
| 188 | "当前环境的模拟宿主结构先在 `platform_mock/operator_data_types.h` 声明" | 补入"实现与注册"一节开头（该节目前没有这一句） |
| 203-206 | `CompanyString` / `CompanyBuffer`、借用指针不跨调用保存 | "宿主调用与生命周期"一节 |
| 240-245 | 单次批次上限、`resolve-conf --depth` 查询 | 保留在第 6 节 |
| 246-249 | 每帧必需槽、输出 key 预置为 null `shared_ptr`、后缀查询 | "宿主调用与生命周期" |
| 251-255 | 借用输入、输出租约、不要累积租约 | "宿主调用与生命周期" |
| 257-260 | 销毁顺序 | "宿主调用与生命周期" |
| 262-265 | Init/DeInit、handle 并发 | "宿主调用与生命周期" |
| 267-274 | `out_mem` 覆盖、`-4` 与回滚、改容量后重建 handle | 保留在第 6 节 |
| 276-280 | 门面错误码、`GetOperatorLastError()` | "宿主调用与生命周期"；第 6 节只留一句"错误码见 `error_codes.h`，`-4` 表示输出容量不足"（链接到 `../../include/platform_mock/error_codes.h`） |
| 282-289 | `resolve-conf` 示例与 `output_pools` 说明 | 保留在第 6 节 |
| 290-291 | 宿主参考 Operator runner 准备输出 key | "宿主调用与生命周期" |

预计第 6 节从 54 行减到约 25 行；第 4 节从 26 行减到约 4 行。扣除第 7 节新增的快速验证内容后，
全文净减约 50 行（约六分之一），并且不再出现租约、销毁顺序、DeInit 等宿主层概念。

### 3.5 operator_output_allocation.md 的改动

- 标题改为"Operator 宿主类型、输出池与生命周期"。文件名不变，避免改动现有链接。
- 开头增加读者说明："本文写给新增宿主类型、特殊输出布局，以及直接调用 SDK 的宿主集成方。
  只复用已有类型的业务开发者见业务接入指南。"（链接到 `business_onboarding.md`）
- "实现与注册"一节开头补入 3.4 中第 188 行那一句。
- 文末新增"## 宿主调用与生命周期"一节，按以下顺序放入 3.4 中移来的段落：
  调用前提（每帧槽位、空输出 key）→ 借用输入与输出租约 → 销毁顺序 → Init/DeInit 与并发 →
  错误码与 `GetOperatorLastError()` → 参考实现（Operator runner、OCR Demo）。
- 第 179 行的链接改为同文档锚点 `#宿主调用与生命周期`。

### 3.6 链接更新

| 文件 | 行 | 原链接 | 新链接 |
| --- | --- | --- | --- |
| `doc/architecture.md` | 157 | `dev_guide/business_onboarding.md#6-输出容量与生命周期` | `dev_guide/operator_output_allocation.md#宿主调用与生命周期` |
| `doc/developer_guide.md` | 51 | 同上 | 同上 |
| `doc/dev_guide/operator_output_allocation.md` | 179 | `business_onboarding.md#6-输出容量与生命周期` | `#宿主调用与生命周期` |
| `doc/README.md` | "进阶与参考"表 | — | 新增一行：资料列为"Operator 宿主类型、输出池与生命周期"（链接到 `dev_guide/operator_output_allocation.md`），职责列为"新宿主类型、特殊输出布局与宿主调用规则" |

指向 `#输入输出以-operator-接口为边界`（6 处）和 `#5-统一-demo-接入`（1 处）的链接保持不变。

### 3.7 拆分"快速反馈"一节（WI-8c）

`tests/README.md:36-64` 的 "Fast feedback for solution authors" 按读者拆开，改写为中文：

| 原内容 | 去向 |
| --- | --- |
| 表格中 Node 一行、构建与运行命令（48-52）、Node helper 与 `--write-test` 的说明（54-62） | `src/custom_nodes/README.md` 新增"## 本地快速验证"一节 |
| 表格中 Adapter 与 Demo 两行 | `doc/dev_guide/business_onboarding.md` 第 7 节 |
| 表格中 Core 一行（Init/Process 诊断与计划） | 留在 tests/README，供框架维护者使用 |

- tests/README 在原位置保留一句英文指引，指向上述两个中文位置。
- `doc/dev_guide/custom_node_concepts.md:248` 的链接 `../../tests/README.md#fast-feedback-for-solution-authors`
  改为 `../../src/custom_nodes/README.md#本地快速验证`。

### 3.8 外部契约影响

无，只改文档和一个检查脚本。

### 3.9 验收

- [ ] `DocLinksTest`、`DocLinksSelfTest` 通过；移动内容之前先在基线上运行，结果为 0 处失效。
- [ ] `business_onboarding.md` 中不再出现"租约"、`DeInit`、`GetOperatorLastError`、
      `RegisterOperatorValueType`、`DECLARE_EXTERNAL_TYPE_TRAITS`（用 `grep` 验证）；
      这些概念都能在 `operator_output_allocation.md` 中找到。
- [ ] 第 4 节原有的每条规则在 `operator_output_allocation.md` 中都有对应内容（PR 中附逐条对照表）。
- [ ] `ArchitectureDocsDriftTest`、`GovernanceConsistencyTest` 通过；统一门禁通过。

回退：还原本 PR；链接检查可单独保留。

---

## WI-4 签名编译期提示（含 WI-8a）

### 4.1 现状

在基线上，用 `edgeflow_capability_nodes_objects` 的编译参数逐个编译以下改动后的 starter：

| 错误写法 | 结果 |
| --- | --- |
| Batch：Options 与 Models 参数对调 | 1 个错误，指向 `function_node.h:144` 的 `return fn(inputs, params)`，提示"invalid initialization of reference"，没有说明正确签名 |
| Batch：返回 `TextBatch` 而不是 `NodeResult<TextBatch>` | 5 个错误，大部分位于 `function_node.h:1548` 一带的模板内部 |
| LLM：`BuildPrompt(std::string&)` | 1 个错误，"discards qualifiers"，能定位回用户文件 |
| LLM：参数按值、返回 `NodeResult<std::string>` 或 `const char*` | 可以编译 |
| LLM：`FormatAnswer` 返回 `char`、`int`、`std::string_view` 或 `NodeResult<std::string_view>` | 都能编译。返回值通过 `std::string` 的赋值运算写入（`function_node.h:1656-1669`），`int` 会被截断成单个字符，属于隐蔽错误 |
| LLM：`BuildPrompt` 返回 `std::string_view`、`char`、`int` 或 `NodeResult<std::string_view>` | 都编译失败，报错在标准库内部。返回值经 `TraceableItem(uint32_t, uint32_t, T)`（`include/contracts/traceable_item.h:25`，调用处 `function_node.h:1618-1633`）构造，要求能隐式转换为 `std::string` |

可见两个钩子接收返回值的方式不同：`BuildPrompt` 要求能隐式转换为 `std::string`，`FormatAnswer`
只要求能赋值给 `std::string`。对应的类型萃取结果：

| 返回类型 | `is_constructible<std::string, R>` | `is_convertible<R, std::string>` | `is_assignable<std::string&, R>` |
| --- | --- | --- | --- |
| `std::string`、`const char*` | 是 | 是 | 是 |
| `std::string_view` | 是 | 否 | 是 |
| `char`、`int` | 否 | 否 | 是 |

因此不能用单一的"可构造出 `std::string`"去判断：对 `BuildPrompt`，它会放过 `std::string_view`，
函数体随后仍然编译失败；对 `FormatAnswer`，它会拒绝现在能编译的 `char`、`int`，构成没有声明的收紧。
| Map：参数为非 const 引用 | 已有 static_assert 给出期望签名（`function_node.h:96`），之后还有 2 个连带错误 |

相关源码：`InvokeMapItem`（`include/nodes/function_node.h:89-101`）、`InvokeBatch`（119-146）、
`MapSpec` 中类作用域的 static_assert（170-172，可作为本项的写法参照）、`BatchSpec`（1060-）、
`MakeBatchSpec` 四个重载（1243-1281）、`AuthorNode<BatchSpec>::ProcessNode`（1525-1567）、
`MakeLlmTextSpec`（1591-1687）。

### 4.2 可接受的签名

签名只在 `function_node.h` 的 static_assert 文案中定义一次。WI-5 的速查表照抄这些签名，
4.4 的测试检查两处一致。

| 写法 | 可接受的签名 | 返回值 |
| --- | --- | --- |
| Batch | `NodeResult<OutputBatch> Run(const Inputs&, const Params&)`<br>`NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&)`<br>`NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&, const SessionResources&)` | `NodeResult<OutputBatch>`；多输出时为 `NodeResult<Outputs>`。没有 `Parameters<...>` 时，`Params` 是 `NoParameters` |
| LLM | `std::string BuildPrompt(const std::string&)`<br>`std::string BuildPrompt(const std::string&, const Params&)`<br>`FormatAnswer` 同形 | 两个钩子规则相同：能隐式转换为 `std::string` 的类型（`std::string`、`const char*`、字符串字面量）、`std::string_view`，或这些类型的 `NodeResult`。不接受 `char`、`int` 等算术类型 |
| Map | 不变：`(const InPayload&)` 或 `(const InPayload&, const Params&)` | 不变 |

`InvokeBatch` 还接受参数相同的成员函数指针（const 与非 const 都可以，见 `MemberFunctionTraits`，
`function_node.h:103-117`）。目前只有测试在用（`tests/unit/nodes/test_function_node.cpp:112`、`:392`），
所以检查必须继续接受这种形式，判断方式与 `InvokeBatch` 现有的 `std::is_invocable_v<Fn, ClassT, ...>`
保持一致；但速查表和报错文案不列出它，避免给业务开发者增加概念。

### 4.3 function_node.h 的改动

**Batch 签名判定**（detail 命名空间，`InvokeBatch` 之后）：

```cpp
template <typename Fn, typename InputsT, typename ParamsT, typename ModelsT>
struct BatchRunSignature {
  // 与 InvokeBatch 的分派条件一一对应：
  // 自由函数/lambda 的 4、3、2 参数形式，以及成员函数指针的同样三种形式。
  static constexpr bool kCallable = /* 任一形式可调用 */;
  using Result = /* kCallable 时为 InvokeBatch 的返回类型，否则为 void */;
};
```

**BatchSpec 类作用域断言**（参照 MapSpec 的写法）：

```cpp
static_assert(
    detail::BatchRunSignature<RunFnT, InputsT, ParamsT, ModelsT>::kCallable,
    "Batch Run must be callable as one of: "
    "NodeResult<OutputBatch> Run(const Inputs&, const Params&) | "
    "NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&) | "
    "NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&, "
    "const SessionResources&). Without Parameters<...>, Params is "
    "NoParameters. See doc/dev_guide/custom_node_concepts.md");
static_assert(
    !kCallable || /* Result 是 NodeResult<T>，且 T 可转换为 OutputBatchT */,
    "Batch Run must return NodeResult<OutputBatch> "
    "(NodeResult<Outputs> for multiple outputs)");
```

`BatchSpec` 增加 `static constexpr bool kRunSignatureValid`，值为上面两个条件的合取。

**消除连带报错**：

- `InvokeBatch` 中自由函数与成员函数两个分支的最后一个 `else`，分别改为
  `else if constexpr (<2 参数形式可调用>)`，不再无条件调用 `fn(inputs, params)` 或
  `(logic.*fn)(inputs, params)`。
- `AuthorNode<BatchSpec>::ProcessNode` 用 `if constexpr (SpecType::kRunSignatureValid)`
  包住现有函数体；`else` 分支只返回失败码。该分支不会被编译进可运行程序，因为签名不合法时
  编译已经失败；它的作用是让 static_assert 成为唯一的报错。

**LLM 钩子**：先统一两个钩子接收返回值的方式，再加断言。

1. 统一转换。在 detail 命名空间新增：

   ```cpp
   // Text accepted from BuildPrompt / FormatAnswer: anything implicitly
   // convertible to std::string, plus std::string_view (converted explicitly).
   template <typename R>
   inline constexpr bool kIsHookText =
       std::is_convertible_v<R, std::string> ||
       std::is_same_v<std::decay_t<R>, std::string_view>;

   template <typename R>
   std::string ToHookText(R&& value) {
     return std::string(std::forward<R>(value));
   }
   ```

   `run_fn` 中两个钩子都先取出返回值（`NodeResult` 先取 `value()`），再用 `ToHookText` 转成
   `std::string`：`BuildPrompt` 的结果交给 `prompts.emplace_back(req_id, sub_id, std::move(text))`，
   `FormatAnswer` 的结果执行 `item.data = std::move(text)`。原来"构造"和"赋值"两种接收方式合并为一种。
   `FormatAnswer` 返回指向原文的 `string_view` 时，先复制再赋值，避免自赋值时的别名问题。

2. 断言。在 `MakeLlmTextSpec` 通用重载（1591 行）开头增加：

   ```cpp
   static_assert(
       std::is_invocable_v<const BuildPromptFn&, const std::string&> ||
           std::is_invocable_v<const BuildPromptFn&, const std::string&,
                               const ParamsT&>,
       "BuildPrompt must be callable as std::string BuildPrompt(const "
       "std::string&) or std::string BuildPrompt(const std::string&, const "
       "Params&)");
   // 返回值为 R 或 NodeResult<R>，且 detail::kIsHookText<R> 成立
   static_assert(/* ... */,
                 "BuildPrompt must return text: std::string, const char*, "
                 "std::string_view, or NodeResult of one of them; char and "
                 "integer results are rejected");
   // FormatAnswer 同样两条，文案中的函数名替换为 FormatAnswer
   ```

3. 行为变化。这是作者接口约束的调整，写入 CHANGELOG（见 4.6）：

   | 返回类型 | `BuildPrompt` 现在 | `FormatAnswer` 现在 | 改动后（两个钩子相同） |
   | --- | --- | --- | --- |
   | `std::string`、`const char*` 及其 `NodeResult` | 可编译 | 可编译 | 接受，行为不变 |
   | `std::string_view` 及其 `NodeResult` | 编译失败（标准库内部报错） | 可编译 | 接受；对 `BuildPrompt` 是放宽 |
   | `char`、`int` 等算术类型及其 `NodeResult` | 编译失败（标准库内部报错） | 可编译，结果被当作单个字符 | 拒绝，报 static_assert；对 `FormatAnswer` 是**收紧** |

   现有代码不受影响：仓库中的 LLM 钩子（`dev_support/node_authoring/starter_llm_node.cpp`，
   `tests/unit/nodes/test_function_node.cpp:410-475` 中的 lambda 与函数对象）都返回 `std::string`
   或 `NodeResult<std::string>`。

   不采用"按两个钩子各自的现有规则分别判断"：那样会保留 `FormatAnswer` 返回整数被静默截断的问题，
   两个钩子的规则也不一致，速查表无法用一句话说明。

`run_fn` lambda 的函数体用 `if constexpr (<四个条件都满足>)` 包住，避免签名错误时
lambda 内部产生连带报错。

其余三个 `MakeLlmTextSpec` 重载都转发到通用重载，不需要单独加断言。

### 4.4 编译失败契约测试

**测试样例**放在 `tests/fixtures/spec_signatures/`。每个文件第一行写预期：

| 文件 | 第一行 | 内容 |
| --- | --- | --- |
| `valid_signatures.cpp` | `// expect-ok` | 合法的 Map、LLM、Batch。Batch 覆盖 `NoParameters`、`SessionResources`、const 与非 const 成员函数指针；LLM 覆盖返回 `std::string`、`const char*`、`std::string_view`、`NodeResult<std::string_view>` 的两个钩子 |
| `batch_params_swapped.cpp` | `// expect-error: Batch Run must be callable as one of` | Options 与 Models 参数对调 |
| `batch_nonconst_inputs.cpp` | `// expect-error: Batch Run must be callable as one of` | `Run(Inputs&, ...)` |
| `batch_missing_noparameters.cpp` | `// expect-error: Batch Run must be callable as one of` | 没有 Parameters 时漏写 `const NoParameters&` |
| `batch_returns_plain_batch.cpp` | `// expect-error: Batch Run must return NodeResult<OutputBatch>` | 返回 `TextBatch` |
| `llm_build_prompt_nonconst.cpp` | `// expect-error: BuildPrompt must be callable as` | `BuildPrompt(std::string&)` |
| `llm_format_answer_returns_int.cpp` | `// expect-error: FormatAnswer must return` | 返回 `int`（基线上能编译，属于收紧） |
| `llm_format_answer_returns_char.cpp` | `// expect-error: FormatAnswer must return` | 返回 `char`（基线上能编译，属于收紧） |
| `llm_build_prompt_returns_int.cpp` | `// expect-error: BuildPrompt must return` | 返回 `int`（基线上在标准库内部报错，改为第一条即断言文案） |

**测试脚本** `tests/contract/authoring/test_spec_signature_diagnostics.cmake`，沿用
`tests/contract/architecture/test_layer_header_views.cmake` 的做法：读取
`compile_checks_<CONFIG>.cmake` 编译清单，用 `capability_nodes_includes` 和
`capability_nodes_definitions` 拼出编译参数，对每个样例执行
`<layer_cxx> -std=c++17 -fsyntax-only <flags> <file>`，然后判定：

- `expect-ok`：必须编译成功。这一条用来防止因头文件路径等无关原因失败而被误判为通过。
- `expect-error`：必须编译失败，并且输出中第一条含 `error:` 的行包含预期文案。不满足时，
  把完整编译输出写进失败信息。
- 防漂移（WI-5）：用正则从 `include/nodes/function_node.h` 中提取全部
  `NodeResult<OutputBatch> Run(...)` 和 `std::string BuildPrompt|FormatAnswer(...)` 签名，
  逐一检查 `doc/dev_guide/custom_node_concepts.md` 是否包含。签名只以头文件为准，脚本里不再抄一份。
- 防空跑：样例目录中 `expect-error` 样例少于 8 个、`expect-ok` 样例少于 1 个，或从头文件提取到的签名
  少于 7 条（Batch 3 条，`BuildPrompt`、`FormatAnswer` 各 2 条）时，直接判失败。这样正则写错或
  样例目录路径错误时，测试不会因为"什么都没检查"而通过。

**CTest 注册**（`tests/RuntimeTests.cmake`，放在 `LayerGuardTest` 旁）：

```cmake
add_test(NAME SpecSignatureDiagnosticsTest
  COMMAND ${CMAKE_COMMAND}
          "-DLAYER_COMPILE_MANIFEST=${PROJECT_BINARY_DIR}/layer_includes/compile_checks_$<CONFIG>.cmake"
          "-DFIXTURE_DIR=${PROJECT_SOURCE_DIR}/tests/fixtures/spec_signatures"
          "-DFUNCTION_NODE_HEADER=${PROJECT_SOURCE_DIR}/include/nodes/function_node.h"
          "-DCONCEPTS_DOC=${PROJECT_SOURCE_DIR}/doc/dev_guide/custom_node_concepts.md"
          -P "${PROJECT_SOURCE_DIR}/tests/contract/authoring/test_spec_signature_diagnostics.cmake")
set_tests_properties(SpecSignatureDiagnosticsTest PROPERTIES
  WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
  LABELS "tier1;static-gate;dev-fast;sanitizer-compatible")
```

`tests/README.md` 的目录说明中补充 `contract/authoring/`：保护节点作者接口的编译期契约。

### 4.5 WI-8a：测试未被调度时的报错文案

`tests/contract/architecture/test_test_labels_contract.py:261-264` 的报错改为：

```python
f"Compiled test '{test}' in '{exe.name}' is not covered by any CTest filter. "
"Add its suite to a filter in tests/RuntimeTests.cmake "
"(edgeflow_add_runner_test), or move the case into an existing suite "
"such as CustomNodeCatalogTest."
```

原有子串 "is not covered by any CTest filter" 保留，第 462 行的自测断言不需要修改。

### 4.6 文档与外部契约影响

`doc/CHANGELOG.md`（Unreleased）：

> Node 作者接口：Batch `Run`、`BuildPrompt`、`FormatAnswer` 签名不符时，编译期直接给出可接受的签名。
> 两个 LLM 钩子统一返回值规则：接受 `std::string`、`const char*`、`std::string_view` 及其 `NodeResult`。
> 约束收紧：`FormatAnswer` 不再接受 `char`、`int` 等算术类型返回值（此前会被当作单个字符写入结果）。
> 约束放宽：`BuildPrompt` 可以返回 `std::string_view`。

外部契约：无影响。上述收紧和放宽都属于源码级作者接口，是内部约定的调整，已列入 RFC 3.2。
接受的返回类型在运行时的行为不变；`FormatAnswer` 返回 `string_view` 时改为先复制再写入，结果相同。

### 4.7 验收

- [ ] `SpecSignatureDiagnosticsTest` 通过：8 个错误样例的第一条报错都是对应的 static_assert 文案，
      对照样例编译成功；防空跑检查生效（临时清空样例目录后测试失败）。
- [ ] 全部生产 Node、`dev_support/node_authoring/` 下的 starter、脚手架生成的夹具
      （`ScaffoldFixtures.cmake`）都能编译；0.1 的比较通过。
- [ ] 在 PR 中贴出 4.1 表中两个 Batch 场景改动前后的编译输出对比。
- [ ] `TestLabelsContractTest` 通过；统一门禁通过。

回退：还原本 PR。

---

## WI-5 签名速查与教程精简

### 5.1 速查表

在 `doc/dev_guide/custom_node_concepts.md` 的引言（1-8 行）之后插入一节"三种写法速查"。拟写内容：

| 写法 | 适用场景 | 工厂 | 业务函数签名 | 返回 |
| --- | --- | --- | --- | --- |
| Map | 逐项纯计算，数量与来源不变 | `MakeMapSpec(Input<InBatch>, Output<OutBatch>, [Parameters<P>], &Transform)` | `Out Transform(const In&)`<br>`Out Transform(const In&, const Params&)` | `Out` 或 `NodeResult<Out>` |
| LLM 两函数 | 文本前处理 → 一次生成 → 文本后处理 | `MakeLlmTextSpec(Input<TextBatch>, Output<TextBatch>, [Parameters<P>], &BuildPrompt, &FormatAnswer)` | `std::string BuildPrompt(const std::string&)`<br>`std::string BuildPrompt(const std::string&, const Params&)`<br>`std::string FormatAnswer(const std::string&)`<br>`std::string FormatAnswer(const std::string&, const Params&)` | 文本：`std::string`、`const char*`、`std::string_view`，或它们的 `NodeResult`；不接受 `char`、`int` |
| Batch | 多输入多输出、拆分聚合、多模型、会话缓存 | `MakeBatchSpec(InputsOf<Inputs>, <输出声明>, [Parameters<P>], [ModelsOf<M>], &Run)` | `NodeResult<OutputBatch> Run(const Inputs&, const Params&)`<br>`NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&)`<br>`NodeResult<OutputBatch> Run(const Inputs&, const Params&, const Models&, const SessionResources&)` | `NodeResult<OutputBatch>`；多输出为 `NodeResult<Outputs>` |

表后加两条说明：

- 没有 `Parameters<...>` 时，`Params` 是 `NoParameters`。Batch 的 `Run` 仍然要写这个参数，
  例如 `Run(const Inputs&, const NoParameters&, const Models&)`，见多模型示例
  （链接到 `../../dev_support/node_authoring/starter_multi_model_node.cpp`）。
- 返回失败用 `NodeResult<T>::Failure(...)`，成功用 `NodeResult<T>::Success(...)`。

### 5.2 常见编译错误对照

紧接速查表：

| 报错包含 | 原因 | 处理 |
| --- | --- | --- |
| `Map function must accept either` | `Transform` 的参数不是 `const In&`（可再带 `const Params&`） | 改成表中签名 |
| `Map function return payload type must match` | 返回类型与输出批的元素类型不同 | 返回 `Out` 或 `NodeResult<Out>` |
| `BuildPrompt must be callable as` / `FormatAnswer must be callable as` | 参数不是 `const std::string&`（可再带 `const Params&`） | 改成表中签名 |
| `BuildPrompt must return` / `FormatAnswer must return` | 返回了非文本类型，例如 `char`、`int` | 返回 `std::string`（或 `const char*`、`std::string_view`），失败时返回 `NodeResult<std::string>` |
| `Batch Run must be callable as one of` | 参数顺序不对、少了 const 引用，或漏写 `const NoParameters&` | 改成表中三种形式之一 |
| `Batch Run must return NodeResult<OutputBatch>` | 直接返回了批次 | `return NodeResult<OutputBatch>::Success(std::move(output));` |

### 5.3 精简"第一个自定义 Node"第 3 节

`doc/dev_guide/first_custom_node.md:84-94` 的整段改为：

```markdown
这里没有模板语言、参数解析或 Markdown 解析器；需要时再使用已有通用节点。
需要多个输入、条件二次推理、多种模型能力或拆分聚合时，改用 Batch 写法：三种写法的签名与
常见编译错误见[写法速查](custom_node_concepts.md#三种写法速查)，第 7 节列出了对应的示例。
外部请求的字段选择与响应组装属于 Adapter，见[输入输出边界](business_onboarding.md#输入输出以-operator-接口为边界)。
```

原段中逐项列出的 API 名（`InputsOf`、`Parameters`、`ModelsOf`、`WithControls`、`WithControl`、
`OutputsOf`、`Produced`、`ProducedBatch`、`PortFlow`）已经在 `custom_node_concepts.md` 和
第 7 节的表格中出现，这里不再重复。

### 5.4 删除写死的数量

| 文件 | 行 | 改为 |
| --- | --- | --- |
| `doc/architecture.md` | 201 | "全部 12 个生产 Node" → "所有生产 Node" |
| `doc/dev_guide/business_onboarding.md` | 168 | "参与编译和测试的八个业务" → "参与编译和测试的现有业务" |
| `doc/dev_guide/custom_node_concepts.md` | 212 | "12 个生产 Node 都使用这套 Spec" → "所有生产 Node 都使用这套 Spec" |
| `doc/dev_guide/first_custom_node.md` | 92 | 随 5.3 一起删除 |
| `src/custom_nodes/README.md` | 12 | "全部 12 个生产 Node" → "所有生产 Node" |

### 5.5 外部契约影响

无，只改文档。

### 5.6 验收

- [ ] `SpecSignatureDiagnosticsTest` 的防漂移检查通过（速查表包含头文件中的全部签名）。
- [ ] `git grep -n -e '12 个' -e '八个业务' -- doc src/custom_nodes` 没有结果。
- [ ] 脚手架夹具生成器仍能从 `first_custom_node.md` 提取 `starter-example` 片段
      （`CustomNodeScaffoldTest` 通过）。
- [ ] `DocLinksTest` 通过（如果 WI-3 已合入）。

回退：还原本 PR。

---

## WI-6 CODEOWNERS

### 6.1 文件内容（草案）

新增 `.github/CODEOWNERS`：

```text
# 审查边界：框架层改动须由框架维护者审查（AGENTS.md：只有证明存在缺口时才改
# Core、Model、Backend）。业务团队接入后，在"业务目录"段为各自路径追加团队，
# 例如：/src/custom_nodes/ @chamsechan @org/team-a
# 规则自上而下匹配，后出现的规则优先。

# 默认
*                                       @chamsechan

# 框架层
/include/core/                          @chamsechan
/include/contracts/                     @chamsechan
/include/nodes/                         @chamsechan
/include/engine/                        @chamsechan
/include/edgeflow/                      @chamsechan
/include/platform_mock/                 @chamsechan
/include/adapter/                       @chamsechan
/src/core/                              @chamsechan
/src/engine/                            @chamsechan
/src/common_nodes/                      @chamsechan
/src/cli/                               @chamsechan
/src/adapter/operator/                  @chamsechan
/cmake_ext/                             @chamsechan
/CMakeLists.txt                         @chamsechan
/CMakePresets.json                      @chamsechan
/scripts/                               @chamsechan
/tests/contract/                        @chamsechan
/dev_support/                           @chamsechan
/tools/                                 @chamsechan
/.github/                               @chamsechan
/.agents/                               @chamsechan
/.codex/                                @chamsechan
/AGENTS.md                              @chamsechan
/CONTRIBUTING.md                        @chamsechan

# 业务目录
/src/custom_nodes/                      @chamsechan
/src/adapter/input/                     @chamsechan
/src/adapter/output/                    @chamsechan
/src/adapter/biz/                       @chamsechan
/include/adapter/biz_blackboard_keys.h  @chamsechan
/include/adapter/biz_results.h          @chamsechan
/demo/biz/                              @chamsechan
/demo/profiles*.json                    @chamsechan
/configs/                               @chamsechan
/data/                                  @chamsechan
```

`src/adapter/` 根目录下的机制文件（`deployment_*`、`io_*` 等）由默认规则覆盖。
两个业务头文件写在 `/include/adapter/` 之后，因此由业务段的规则决定审查人。

### 6.2 治理检查

`scripts/check_governance.sh` 的 `required_files` 增加 `.github/CODEOWNERS`，并检查文件中
包含 `/include/core/`、`/src/core/`、`/src/engine/` 三条框架规则，防止框架边界被误删。

### 6.3 CONTRIBUTING 补充

§7 末尾增加一句：

> Framework paths listed in `.github/CODEOWNERS` require review by their owners once code-owner
> review is enabled in branch protection.

### 6.4 仓库设置（手工）

第二个团队接入时，维护者在 GitHub 分支保护中开启"Require review from Code Owners"，并在业务段
追加该团队。迁入内网平台时，按该平台的 CODEOWNERS 位置约定调整。例如 GitLab 读取根目录、
`docs/` 或 `.gitlab/` 下的文件。

### 6.5 外部契约影响

无。

### 6.6 验收

- [ ] GitHub 的 CODEOWNERS 校验没有报错（PR 页面不提示无效语法或无效用户）。
- [ ] `GovernanceConsistencyTest` 通过；删掉 `/src/core/` 规则后，该测试失败。

回退：还原本 PR。

---

## WI-7 整理计划文件

### 7.1 编辑清单（`plans/FRAMEWORK_SIMPLIFICATION_PLAN.md`）

| 原章节 | 处理 |
| --- | --- |
| 开头说明（1-5 行） | 在后面加状态表：阶段 1、2 → 由 `DEVELOPER_EXPERIENCE_RFC.md`（WI-1、WI-4）取代；阶段 3 → 已完成（合入于 PR #150）；阶段 4 → 设计待评审；阶段 5 → 已完成（PR #150–#154） |
| §1 目标与约束 | 保留：阶段 4 仍受这些约束 |
| §2 现状核对 | 只保留"阶段"列为 4 的行，其余删除 |
| §3 总览与顺序 | 表格改为只列阶段 4；删除顺序理由中关于阶段 1、2、3、5 的条目 |
| §4 阶段 0 基线 | 删除：方法已写进本设计 0.1 |
| §5 阶段 1、§6 阶段 2 | 删除，留一行指向 RFC |
| §7 阶段 3（含 7.7 实施记录） | 删除：已完成；规则已写进现行指南，过程在 Git 和 PR 中 |
| §8 阶段 4 | 保留 |
| §9 不做的事项及理由 | 保留：RFC 3.3 引用了它 |
| §10 对讨论的修正 | 删除：结论已并入 §8，属于开发过程记录 |
| §11 完成后的效果 | 只保留阶段 4 一行 |
| §12 阶段 5 | 删除正文；"待方案负责人确认"的两项（rerank 段落上限、显式默认值字段）已移到 RFC 第 10 节 |
| 附录：配置变体差异 | 保留：属于方案负责人的待决信息 |

### 7.2 验收

- [ ] 文件中不再出现"尚未提交"；阶段状态与 Git 历史一致。
- [ ] `DocLinksTest` 通过（如果已合入）。

回退：还原该提交。

---

## O-1 示例配置删除等于默认值的显式字段（待决定）

**范围**：`configs/` 与 `demo/fixtures/mock/` 下的 Pipeline JSON。计划第 12 节的基线统计为
161 个与 29 个字段。

**步骤**：

1. 用提交 `12b3345` 中 `doc/platform_parameter_ownership_plan.md` 附录 A.3 的
   `report_explicit_defaults.py` 生成清单（`git show 12b3345:doc/platform_parameter_ownership_plan.md`）。
2. 方案负责人在清单中标出"有意固定"的字段，其余字段删除。
3. 对每份修改过的配置，比较修改前后 `alg_pipeline_tool resolve-conf` 输出中的
   `effective_pipeline`，必须完全一致。计划第 10 节说明了为什么不能只比较 `plan`。
4. 0.1 的比较通过（catalog 与 smoke 结果都不变）。

**外部契约影响**：Pipeline JSON 格式不变，只删除取值等于默认值的字段，生效配置逐份证明不变。

**风险**：框架以后修改默认值时，这些配置会随之变化。因此"有意固定"的字段必须保留。

## O-2 CONTRIBUTING 直接改为中文（待决定）

**做法**：原地改写为中文，只保留一份，章节结构和编号不变。

**锚点**：英文标题的锚点会变化，需要更新 9 处入链：
`#3-design-and-current-contracts` 6 处、`#6-run-one-canonical-delivery-gate` 2 处、
`#5-update-durable-documentation-proportionally` 1 处，分布在 7 个文件中（含 AGENTS.md 与 Skill）。
`DocLinksTest` 负责发现遗漏。

**治理检查**：`check_governance.sh` 中的旧术语黑名单与语言无关，不需要修改。

**外部契约影响**：无。

---

## 附录 A：基线证据与复现

以下结果均在基线 `main@26d60f2` 的现有构建目录 `build/`（dev-gate 预设）上得到。

| 项 | 复现方法 | 结果 |
| --- | --- | --- |
| 快速开始 | `./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json`；`./build/alg_demo --profile keyword_match_rules --dataset tests/fixtures/effects/keyword_inputs.txt --output-dir <dir>` | 校验通过；4 条结果，前两条命中 `SYSTEM_INIT`，与 README 一致 |
| 生产工具校验 mock 配置 | 见 2.1 | 3 条诊断，其中 1 条为连带误报 |
| 节点类型拼写错误 | 把关键词配置中的 `TextRuleMatchNode` 改为 `TextRuleMatchNod` 后校验 | 3 条诊断，其中 2 条为连带的 `MISSING_BIZ_OUTPUT` |
| 未知配置字段 | 把 `categories` 改为 `categoriess` 后校验 | `candidate_fields` 按相似度排序（`categories` 在首位）；顶层 `suggestions` 按 schema 顺序（`default_category` 在首位） |
| Batch 签名错误 | 从 `build/compile_commands.json` 取 `prompt_guided_llm_node.cpp` 的编译命令，把源文件换成修改后的 `starter_batch_node.cpp` | 见 4.1 |
| LLM 与 Map 签名错误 | 同上，分别使用 `starter_llm_node.cpp` 和脚手架生成的 Map 节点 | 见 4.1 |
| Markdown 链接 | 原型检查脚本（规则同 3.2） | 53 个文件，59 处跨文件锚点链接，0 处失效 |
| SDK 报错路径 | 阅读 `src/adapter/io_binding_resolver.cpp:198-210` | 只使用第一条诊断的 code、path、message |
| 注册顺序 | 阅读各注册表实现 | 使用 `unordered_map`；绑定注册不依赖转换器的注册顺序 |
| 未知节点与独立错误并存（`combo_type`） | 在关键词配置中加入三个节点：拼错的 `TextRuleMatchNod`（输出映射到 `shared_key`）、已知的 `TextRuleMatchNode`（输出同样映射到 `shared_key`）、读取 `shared_key` 作为 `text` 的 `TextRuleMatchNode` | `UNKNOWN_NODE_TYPE` + `/pipeline/3/inputs/text` 的 `MISSING_INPUT_PRODUCER`；后者是独立的类型错误 |
| 纯连带（`combo_cascade`） | 同上，但去掉已知生产者，下游读取的键只由拼错的节点映射 | `UNKNOWN_NODE_TYPE` + 下游的 `MISSING_INPUT_PRODUCER`；后者是连带结果 |
| LLM 钩子返回类型 | 用同一编译命令编译修改后的 `starter_llm_node.cpp` | `FormatAnswer` 返回 `char`、`int`、`string_view`、`NodeResult<string_view>`：全部能编译；`BuildPrompt` 返回 `string_view`、`char`、`int`、`NodeResult<string_view>`：全部失败；`BuildPrompt` 返回 `const char*`：能编译 |
| 原比较脚本空跑 | 两份采集使用相同的 catalog，都没有 smoke 结果 | 原脚本输出 `identical`，返回 0（假通过）；0.1 的新脚本输出 `FAIL`，返回 1。两份真实采集用新脚本比较，输出 `identical (9 smoke profiles)` |

## 附录 B：受影响文件索引

| 文件 | 项 |
| --- | --- |
| `src/custom_nodes/CMakeLists.txt`、`src/common_nodes/CMakeLists.txt`、`src/adapter/CMakeLists.txt`、`demo/CMakeLists.txt` | WI-1 |
| `tools/scaffold_custom_node.py`、`tools/dev_recipe.py` | WI-1 |
| `tests/tooling/generate_scaffold_fixtures.py`、`tests/ScaffoldFixtures.cmake`、`tests/tooling/test_scaffold_custom_node.py`、`tests/tooling/test_dev_recipe.py` | WI-1 |
| `include/core/remediation_cause.h`、`src/core/pipeline_validator.cpp` | WI-2、WI-8b |
| `src/cli/alg_pipeline_tool.cpp`、`tests/CMakeLists.txt` | WI-2 |
| `tests/integration/pipeline/test_pipeline_catalog_validator.cpp`、`tests/tooling/test_pipeline_studio.py` | WI-2 |
| `scripts/check_doc_links.py`（新增） | WI-3 |
| `include/nodes/function_node.h` | WI-4 |
| `tests/contract/authoring/test_spec_signature_diagnostics.cmake`、`tests/fixtures/spec_signatures/*.cpp`（新增） | WI-4 |
| `tests/RuntimeTests.cmake` | WI-3、WI-4 |
| `tests/contract/architecture/test_test_labels_contract.py` | WI-8a |
| `.github/CODEOWNERS`（新增）、`scripts/check_governance.sh` | WI-6 |
| `doc/dev_guide/business_onboarding.md` | WI-1、WI-3、WI-5、WI-8c |
| `doc/dev_guide/operator_output_allocation.md` | WI-3 |
| `doc/dev_guide/custom_node_concepts.md` | WI-1、WI-5、WI-8c |
| `doc/dev_guide/first_custom_node.md` | WI-1、WI-5 |
| `doc/dev_guide/first_control.md`、`doc/dev_guide/recipe_text_llm_node.md` | WI-1 |
| `doc/architecture.md`、`doc/developer_guide.md`、`doc/README.md` | WI-3、WI-5 |
| `src/custom_nodes/README.md` | WI-1、WI-5、WI-8c |
| `tests/README.md` | WI-1、WI-4、WI-8c |
| `tools/pipeline_studio/README.md` | WI-2 |
| `.agents/skills/edgeflow-node-{map,llm,batch}-developer/SKILL.md`、`.agents/skills/edgeflow-adapter-developer/SKILL.md` | WI-1 |
| `CONTRIBUTING.md` | WI-6；O-2 |
| `doc/CHANGELOG.md` | WI-1、WI-2、WI-4 |
| `plans/FRAMEWORK_SIMPLIFICATION_PLAN.md` | WI-7 |
| `configs/*.json`、`demo/fixtures/mock/*.json` | O-1（待决定） |
