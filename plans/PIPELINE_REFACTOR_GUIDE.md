# Pipeline 配置改造实施总说明

> **状态**：步骤 1–3 已实施；步骤 4–9 待实施。
> **本地交付**：按用户要求在 `refactor/pipeline-config-v2` 上逐步验收、每步一次提交；远程 PR 与合入不在本次授权范围。
> **基线**：`main@cda1f5c`。
> **用途**：把三份设计串成一条可以照着执行的顺序。"改成什么样"以三份设计为准；"按什么顺序做、做到哪、怎么验收、怎么合入"以本文为准。三份设计中关于 PR 划分和门禁的说法与本文不一致时，以本文为准。
> **三份设计**：
> - [Pipeline I/O 改造设计](PIPELINE_IO_DESIGN.md)（下称 I/O 设计）；
> - [Pipeline 模型配置改造设计](PIPELINE_MODEL_DESIGN.md)（下称模型设计）；
> - [Pipeline 节点与连线改造设计](PIPELINE_NODE_DESIGN.md)（下称节点设计）。
>
> 最后一步合入 main 时，删除本文和三份设计。

## 1. 原则

1. **不做兼容**。不保留旧字段别名，不识别旧格式，不提交迁移代码，不为旧名字编写"会被拒绝"的专门测试，旧名字按通用的未知字段规则报错。配置在格式切换的步骤中本地一次性迁移，迁移脚本不提交。
2. **先生产代码，后测试代码**。每个步骤内按以下顺序进行：
   1. 改生产代码，直到生产目标能编译、Demo smoke 能跑通；
   2. 改测试代码，直到过渡期门禁（第 4 节）通过；
   3. 改文档。
3. **工具与可视化推迟**。本轮不改 Pipeline Studio、Python 工具和命令行工具的功能，统一留到第 9 步（待办）。例外见第 2 节。
4. **行为不变**。8 个业务的 Operator 黄金测试期望值一律不改。Demo smoke 的 `results.jsonl` 中 `output` 字段与基线逐条一致。
5. **一步一个 PR，做完再做下一步**。不跨步骤混改；一个 PR 只完成一个步骤。
6. **删除、移动或改名文件时，同一步内改掉所有指向它的文档链接和脚本引用**。设计的文档表列出了已知位置；过渡期门禁中的 `DocLinksTest`、`LayerGuardTest` 会报出遗漏。
7. **旧名字检查的范围**。三份设计验收中的 `git grep` 清单，检查时排除 `plans/` 和 `doc/CHANGELOG.md` 的历史条目：计划文件描述的是各自的基线，不是现行规则（[plans/README](README.md)）。第 1–8 步还排除第 2 节中推迟的工具目录，第 9 步再检查全仓库。

## 2. 代码范围

| 类别 | 目录 | 本轮的处理 |
| --- | --- | --- |
| 生产代码 | `include/`；`src/`（不含 `src/cli/`）；`dev_support/`（测试夹具与 starter 模板，不含 `benchmarks/`、`node_authoring/benchmark/`）；`demo/`；`configs/`；`data/`；`cmake_ext/`；`scripts/` 中的构建、导出检查和下载脚本 | 按步骤修改 |
| 测试代码 | `tests/`，包括 `tests/fixtures/pipelines/`、`tests/support/`；`tests/tooling/` 中只含 `generate_scaffold_fixtures.py`、`test_json_prompt_demo.py` | 每步生产代码完成后修改 |
| 文档 | `AGENTS.md`、`CONTRIBUTING.md`、`doc/`、`configs/README.md`、`src/**/README.md`、`include/platform_mock/README.md`、`.agents/skills/`、`plans/` | 每步最后更新正文；架构图和工具相关说明推迟 |
| 工具与可视化 | `tools/`（Studio、`verify_selection.py`、`dev_recipe.py`）；`src/cli/` 的命令功能；`dev_support/benchmarks/`、`dev_support/node_authoring/benchmark/`（按需运行的性能测量，不在门禁中）；`tests/tooling/` 中的 Python 和浏览器测试（`test_json_prompt_demo.py` 除外，它测的是 Demo）；`tests/fixtures/effects/`；`.github/workflows/ci.yml`；tier4 测试 | 不改，留到第 9 步 |

**推迟期间必须做的最小改动**：
- **`src/cli/`**：
  - 保持能编译；
  - 保持 `alg_pipeline_tool validate` 和 `plan` 可用，用来逐个检查迁移后的方案；
  - 其他命令中依赖已删除概念的分支（例如 `catalog --io-binding`、`init --io-binding`）直接删除，对应命令暂时返回明确的错误，第 9 步再按设计实现。
- **两个例外，必须随步骤修改**：
  - **修复建议库** `src/cli/pipeline_remediation.cpp`：它被 tier3 测试程序链接（`PipelineValidatorTest.Explain*`）。所以每次诊断码变化，它都随所在步骤一起改。
  - **脚手架** `tools/scaffold_custom_node.py` 与 `tests/tooling/generate_scaffold_fixtures.py`：它们在构建时生成的代码和教程方案会编进 tier1 测试程序。生成器还会从 `doc/dev_guide/first_custom_node.md`、`first_control.md` 抽取代码和方案。所以 API 或方案格式变化时，脚手架生成的内容和这两篇文档随所在步骤一起改。脚手架的其他功能和它自己的 Python 测试仍推迟。

## 3. 分支与合入

工具推迟后，格式切换会让 tier4 测试失败；CI 只在 main 上运行（推送到 main，或目标为 main 的 PR）。为了不让 main 的 CI 变红，全部步骤在同一个集成分支上进行：

1. 确认当前分支和工作区状态，从最新的 `origin/main` 新建集成分支 `refactor/pipeline-config`。
2. 每个步骤从集成分支拉出工作分支，完成后提一个 PR 合入集成分支。PR 描述写明：步骤号与对应的设计章节、推迟到第 9 步的内容、本地验收的命令与结果。
3. 集成分支上的 PR 不触发 CI，验收以第 4 节的本地命令为准。
4. 期间 main 有其他合入时，在步骤边界把 main 合并进集成分支。
5. 第 9 步完成，完整门禁和 kite 验证都通过后，集成分支整体提一个 PR 合入 main。

提交信息沿用仓库现有格式，例如 `refactor(core): …`、`feat(adapter): …`。

## 4. 本地验收命令

**开始之前**，在基线（集成分支刚建好时）上运行一次 Demo smoke，结果放在仓库外，作为整个改造的对比基准：

```bash
./scripts/configure_build.sh "$PWD" build dev-gate
cmake --build build -j"$(nproc)"
./build/alg_demo --suite smoke --output-dir "$HOME/edgeflow-smoke-baseline"
```

**生产代码完成时**（每步的第 1 段）：

```bash
cmake --build build -j"$(nproc)" --target alg_sdk alg_demo alg_pipeline_tool alg_show
rm -rf /tmp/edgeflow-smoke && ./build/alg_demo --suite smoke --output-dir /tmp/edgeflow-smoke
```

然后只比较每条记录的 `status` 和 `output`（耗时等字段每次都不同；第 2 步起记录中不再有 `biz`）：

```bash
python3 - "$HOME/edgeflow-smoke-baseline" /tmp/edgeflow-smoke <<'PY'
import json, pathlib, sys
def load(root):
    rows = {}
    for path in sorted(pathlib.Path(root).rglob("results.jsonl")):
        for i, line in enumerate(path.open(encoding="utf-8")):
            record = json.loads(line)
            rows[(record.get("profile"), i)] = (record.get("status"), record.get("output"))
    return rows
base, cur = load(sys.argv[1]), load(sys.argv[2])
diff = sorted(k for k in base.keys() | cur.keys() if base.get(k) != cur.get(k))
print("一致" if not diff else f"不一致：{diff[:10]}")
sys.exit(1 if diff else 0)
PY
```

**测试代码完成后**（过渡期门禁，不含 tier4）：

```bash
./scripts/format.sh --check
git diff --check
./scripts/configure_build.sh "$PWD" build dev-gate
cmake --build build -j"$(nproc)"
(cd build && ctest -j"$(nproc)" --output-on-failure --no-tests=error -LE tier4)
```

**格式切换的步骤**（第 6–8 步）另外要做：
- 用 `./build/alg_pipeline_tool validate <方案>` 和 `plan <方案>` 逐个检查 `configs/`、`demo/fixtures/mock/` 中迁移后的方案；
- kite 方案需要 kite 构建：本地有私有发布时，用 `cmake --preset=kite-cpu` 构建，再运行 `ctest -L kite`；没有条件时，在最后合入 main 时由 CI 的 kite 任务验证。

**完整门禁** `./scripts/run_all_tests.sh` 在第 9 步完成后运行。过渡期间 tier4 失败属于预期，不要为了让它通过而在第 1–8 步中修改工具。

## 5. 步骤总览

| 步骤 | 内容 | 设计依据 | 改配置格式 | 依赖 |
| --- | --- | --- | --- | --- |
| 1 | 参数声明下沉到 `contracts/` | I/O 设计阶段 0 | 否 | 无 |
| 2 | Demo 按宿主结构体运行；SDK 预检返回 I/O 契约 | I/O 设计阶段 1 | 否 | 无 |
| 3 | 模型、后端改用统一参数机制 | 模型设计阶段 1 | 否 | 1 |
| 4 | 节点参数机制：元素类型、`Include`、生成参数、`bind_model` 说明、控制命令由声明生成 | 节点设计阶段 1（5.1–5.4、5.6） | 否 | 1 |
| 5 | 文本处理类节点；删除 8 元组、`ConfigParser`、`WithControl` | 节点设计阶段 1（6.4、5.5） | 只改 `fallback_json` | 3、4 |
| 6 | `io` 格式切换 | I/O 设计阶段 2 | 是 | 1、2 |
| 7 | 模型格式切换 | 模型设计阶段 2 | 是 | 3、4、6 |
| 8 | 节点条目与连线格式切换 | 节点设计阶段 2 | 是 | 5、7 |
| 9 | 待办：工具与可视化；完整门禁；合入 main | 三份设计中的工具部分 | 否 | 1–8 |

第 5 步依赖第 3 步，是因为删除 8 元组构造函数之前，模型和后端必须已经改用 `Field`。

## 6. 各步骤的做法

每步都按"生产代码 → 测试代码 → 文档"的顺序进行，验收用第 4 节的命令。下面列出各段对应的设计内容，以及本步推迟到第 9 步的部分。

### 步骤 1：参数声明下沉到 `contracts/`

依据：I/O 设计第 10 节"阶段 0"。只移动代码、改类名，不改变行为。

- **生产代码**：
  - 新增 `include/contracts/parameters.h`，移入参数声明相关的类型；`NodeConfigParser` 改名为 `ConfigParser`；
  - 删除 `include/nodes/parameter_binding.h`、`node_config_parser.h`；
  - `include/nodes/` 中的相关头文件改包含路径；
  - `src/common_nodes/` 中的 4 个节点、`dev_support/` 中的用法改类名；
  - `FieldJson` 改为 `contracts/config_schema.h` 中的 `ConfigFieldToJson`。
- **测试代码**：
  - 设计中列出的 4 个测试文件和 `test_layer_header_views.cmake`；
  - 脚手架生成的代码若用到旧类名，同步修改。
- **文档**：`doc/dev_guide/custom_node_concepts.md`、`doc/dev_guide/source_layout.md`、`src/custom_nodes/README.md`、`.agents/skills/edgeflow-node-developer/SKILL.md`、`llm-edgeflow-developer-guide/references/capability-nodes.md` 中的类名与头文件路径。
- **推迟**：`dev_support/node_authoring/benchmark/run.py` 的头文件列表。
- **验收**：除第 4 节外，`src/common_nodes`、`src/custom_nodes` 只有类名变化。

### 步骤 2：Demo 按宿主结构体运行，SDK 预检返回 I/O 契约

依据：I/O 设计 8.1、第 9 节、第 10 节"阶段 1"。

- **生产代码**：
  - `include/edgeflow/operator/interface.h`、`src/adapter/operator/operator_adapter.cpp`：新增 `OperatorIoEntry`、`OperatorIoContract`、`ResolveOperatorConfigIo`，删除 `ResolveOperatorConfigBiz`。本步中每个旧槽对应一项，`name` 为原业务名，`service_type` 为空；
  - `cmake_ext/edgeflow_sdk.map.in`、`scripts/check_sdk_exports.sh`：导出符号；
  - `demo/`：
    - 新增 `common/demo_io_registry.{h,cpp}`、`input/*.cpp`、`output/*.cpp`；
    - 修改 `common/operator_runner.h`、`main.cpp`、`common/demo_options.*`、`CMakeLists.txt`、`profiles.json`；
    - 删除 `demo/biz/`、`common/demo_registry.*`；
  - 新增 `data/keyword_match_control.json`。
- **测试代码**：`tests/integration/demo/test_demo_runner.cpp`、`tests/contract/abi/test_cpp_operator_sdk.cpp`、`tests/integration/operator/test_operator_api.cpp`、`tests/unit/adapter/test_io_binding_registry.cpp` 中原 `ResolveOperatorConfigBiz` 的用例。
- **文档**：
  - `doc/dev_guide/business_onboarding.md` 的 Demo 部分；
  - `doc/dev_guide/operator_output_allocation.md` 中指向 `demo/biz/` 的链接；
  - `doc/dev_guide/first_control.md`、`doc/developer_guide.md`；
  - `doc/architecture.md` 的导出函数列表；
  - `.agents/skills/pipeline-composer/references/workflow.md`；
  - `doc/CHANGELOG.md`。
- **推迟**：`doc/architecture_classes.puml` 及重新生成的 SVG。
- **验收**：除第 4 节外，`demo/json_prompt_demo.py` 的测试通过；SDK 导出白名单为 6 个符号。

### 步骤 3：模型、后端改用统一参数机制

依据：模型设计第 5 节、第 11 节中阶段 1 的各行、第 14 节"阶段 1"。配置格式不变。

- **生产代码**：
  - 新增 `include/contracts/parameter_set.h`，本步实现 `Fields()`、`Parse`、`Get<P>()`；`Integer()`、`Effective()` 与 `ParameterFieldBinding::Read` 在第 6 步随 converter 参数加入；
  - `parameters.h` 支持 `std::optional`；删除 `ConfigValueOrDefault`。`.File()` 在第 7 步加入（模型设计 C4）；
  - `include/engine/` 中的 Definition、创建上下文、加载规格改为 `params` 与 `Params<P>()`；工厂只解析一次参数；
  - 全部模型、后端改用四段写法，并删除第 11 节中阶段 1 的各项复查；
  - 新增 `from_model.h`；`bert_model_support.*` 新增读取固定形状的辅助函数；
  - `dev_support/inference/` 中的测试模型、测试后端改用同样的写法；
  - Core 的校验器和 Catalog 改用 `params.Fields()`、`Parse`；
  - `src/cli/pipeline_json_schema.cpp` 只做保持编译所需的修改。
- **测试代码**：`tests/unit/engine/*`、`tests/contract/catalog/*`、`test_layer_header_views.cmake`；模型设计第 15 节中阶段 1 的用例。
- **文档**：
  - `.agents/skills/edgeflow-model-developer`、`edgeflow-backend-developer`；
  - `llm-edgeflow-developer-guide/references/model-execution.md`；
  - `doc/developer_guide.md` 中模型与后端的编写部分。
- **推迟**：无。
- **验收**：
  - 除第 4 节外，方案文件不变；
  - 用 `alg_pipeline_tool describe-model`、`describe-backend` 手动对比，参数名、类型、默认值和范围与改造前一致。例外：`bge_embedding` 的 `embedding_dim`，以及 `bge_embedding`、`bge_reranker` 的 `max_length` 改为可选；whisper、kite 的说明改为中文；
  - `git grep` 不再出现 `ConfigValueOrDefault`、`ParseLlamaCppConfig` 和模型、后端的 `validate_config`。

### 步骤 4：节点参数机制

依据：节点设计 5.1–5.4、5.6，以及第 10 节"阶段 1"中标为第 4 步的各行。配置格式不变；本步中 `llm_generate` 仍是单个提示词，8 元组、`ConfigParser`、`WithControl` 仍保留（第 5 步删除）。

- **生产代码**：
  - `include/contracts/parameters.h`、`config_schema.h`、`config_schema_validation.h`：元素类型（数组、映射、结构体元素、JSON 值）、`items`、`fields`、元素的诊断路径、`Include`、`ConfigFieldJsonSchema`；
  - 新增 `include/nodes/generate_parameters.h`，删除 `generate_options_config.h`；
  - `include/nodes/function_node.h`：`Model(...)` 不再接收说明，由类别生成；
  - `include/nodes/control_authoring.h`：字段命令的格式由声明生成，payload 至少给出一个受控参数；
  - 所有调用 `Model(...)` 或生成参数的节点和 starter：`src/common_nodes/*`、`src/custom_nodes/*`、`dev_support/node_authoring/*`；
  - `src/core/pipeline_validator.cpp`、`src/core/pipeline_catalog.cpp`：元素的诊断路径；Catalog 导出 `items`、`fields`；
  - 修复建议库 `src/cli/pipeline_remediation.cpp`：处理新的参数类型（第 2 节例外）。
- **测试代码**：
  - `tests/unit/nodes/test_parameter_binding.cpp`（元素类型、`Include`）、`test_function_node.cpp`（字段命令）、`test_common_nodes.cpp`、`tests/contract/catalog/*`；节点设计第 11 节"参数声明""生成参数""控制命令"三行的用例；
  - 脚手架生成的代码按新写法修改；`doc/dev_guide/first_custom_node.md`、`first_control.md` 中会被抽取编译的代码同步修改。
- **文档**：`doc/dev_guide/custom_node_concepts.md`、`first_control.md`、`doc/developer_guide.md`、`.agents/skills/edgeflow-node-developer`、`llm-edgeflow-developer-guide/references/capability-nodes.md`、`src/custom_nodes/README.md`。
- **推迟**：脚手架的其他功能及其 Python 测试；`export-schema` 改用 `ConfigFieldJsonSchema`。
- **验收**：除第 4 节外，`describe-node` 的参数名、类型、默认值和范围与改造前一致。例外：`prompt_guided_llm`（本步中仍叫 PromptGuidedLlmNode）的 `max_tokens` 默认值改为 128；数组参数多出元素说明。

### 步骤 5：文本处理类节点

依据：节点设计 6.4、5.5，以及第 10 节"阶段 1"中标为第 5 步的各行。

- **生产代码**：
  - `src/common_nodes/text_template_node.cpp`、`text_rule_match_node.cpp`、`structured_json_parse_node.cpp`、`text_corpus_source_node.cpp`：按 6.4 改用 `Field`；两个控制命令改用字段命令；删除 6.4 列出的输入和参数；
  - `include/core/common_contracts.h` 删除 `TextAttributesBatch`；`include/nodes/node_error_codes.h` 删除 `text_template::kMissingVariable`；
  - `src/adapter/operator/operator_control_registry.cpp`：`kSwitchPrompt` 不再转发 `prompt_id`；
  - 5.5 的删除：`ConfigFieldDefinition` 的 8 元组构造函数、`ConfigParser`、`Parameters::WithParser`、`NodeSpec::WithControl`；
  - 方案：`configs/`、`demo/fixtures/mock/` 中的 `fallback_json` 按 6.4 的迁移改写。
- **测试代码**：节点设计第 10 节"阶段 1"中标为第 5 步的测试文件；`tests/fixtures/pipelines/` 和测试中内嵌的 `fallback_json`；第 11 节"文本处理类"一行的用例；删除只为 8 元组、`ConfigParser`、`WithControl` 而写的用例。
- **文档**：节点设计第 12 节中标为阶段 1 的一行：删除 `WithParser`、`ConfigParser`、`WithControl` 和 `missing_variable_policy` 的说明（`doc/dev_guide/custom_node_concepts.md`、`first_control.md`、`src/custom_nodes/README.md`、两个 skill 文件）。
- **推迟**：`dev_support/benchmarks/control_snapshots.*`（用了 `values` 和 `prompt_id`）。
- **验收**：除第 4 节外：
  - 改动的方案通过 `alg_pipeline_tool validate` 和 `plan`；
  - 节点设计第 10 节"阶段 1"的验收项都满足。

### 步骤 6：`io` 格式切换

依据：I/O 设计第 4–7 节、第 10 节"阶段 2"、第 11 节。

| 提交 | 做法 |
| --- | --- |
| C1 Core 去掉业务 | 按设计；生产代码和测试代码分别提交 |
| C2 converter 登记、参数与输出内存 | 按设计 |
| C3 `io` 文档与部署准备 | 按设计 |
| C4 删除 binding、按（结构体，业务）登记 | 按设计，包括平台模拟中的 `service_type` |
| C5 命令行工具 | **推迟**。本步只做：`src/cli/` 保持编译和 `validate`/`plan` 可用；修复建议库跟随诊断码改名（例如 `missing_output_producer`） |
| C6 Studio 与 Python 工具 | **推迟** |
| C7 配置与测试迁移 | 配置（`configs/`、`demo/fixtures/mock/`）属于生产代码，先迁移；`tests/fixtures/pipelines/` 和测试中内嵌的配置随测试代码迁移。效果规格 `tests/fixtures/effects/` **推迟** |
| C8 文档 | 第 12 节中的正文更新；`tools/pipeline_studio/README.md` 和架构图**推迟**；本步不删除 I/O 设计，第 9 步统一删除 |

- **测试代码**：I/O 设计第 11 节中"工具"一行以外的全部用例。
- **验收**：
  - 除第 4 节外，I/O 设计第 10 节"阶段 2"中除工具以外的验收项都要满足；
  - 设计中的 `git grep` 清单，在 `tools/`、`src/cli/`、`tests/tooling/` 以外不再出现。

### 步骤 7：模型格式切换

依据：模型设计第 4–12 节、第 14 节"阶段 2"、第 15 节。

| 提交 | 做法 |
| --- | --- |
| C1 类别、名字与实现改名 | 按设计；改名顺序见 6.1 |
| C2 节点参数 | 按设计，包括 `normalize` 移到模型、删除 `EmbeddingOptions`、删除 `prompt_prefix` |
| C3 Core 新格式 | 按设计，包括按类别和后端选出实现、注册审计、`dev_support/inference/` 的测试夹具改用 `kFixture` 和 `fixture_backends`。测试源码中自行注册的测试模型（约 10 个文件，模型设计 4.1）随本步的测试代码改写 |
| C4 文件参数与批大小 | 按设计 |
| C5 命令行工具 | **推迟**。本步只做：保持编译和 `validate`/`plan` 可用；修复建议库跟随诊断码与修复原因的变化 |
| C6 Studio 与 Python 工具 | **推迟**，包括资源清单中的选型模板（13.4） |
| C7 目录合并与迁移 | 第 12 节的目录合并：`models/` 并入 `configs/`、`.gitignore`、下载与 e2e 脚本，属于生产代码；按 4.5 迁移方案。CI 中的缓存路径与 kite 任务**推迟**到第 9 步（集成分支上不运行 CI） |
| C8 文档 | 第 16 节中的正文更新；`tools/pipeline_studio/README.md` **推迟** |

- **测试代码**：模型设计第 15 节中"工具"一行以外的全部用例。
- **验收**：
  - 除第 4 节外，模型设计第 14 节"阶段 2"中除工具以外的验收项都要满足；
  - 本地有真实权重时运行 `scripts/run_real_model_e2e.sh`。

### 步骤 8：节点条目与连线格式切换

依据：节点设计第 3、4、6、7、8 节，第 10 节"阶段 2"，第 11 节。

| 提交 | 做法 |
| --- | --- |
| C1 节点类型改名 | 按设计。脚手架要改为生成 snake_case 名字，生成的教程方案改为新格式，`first_control.md`、`first_custom_node.md` 同步修改；它们会编进 tier1 测试程序（第 2 节例外） |
| C2 Core 条目与连线 | 按设计，包括诊断码、控制命令信封、边界伪节点统一为 `input`/`output`、生命周期跟随输入 |
| C3 io 端口 | 按设计：converter 逻辑端口、输出项 `inputs`、删除 `biz_blackboard_keys.h`、改名为 `input_limits.h` |
| C4 节点 | 按设计：`llm_generate` 的 endpoints、三个检索节点 |
| C5 命令行工具 | **推迟**，包括 `edit` 的新操作（4.5）。本步只做：保持编译和 `validate`/`plan` 可用；修复建议库跟随 4.4 的诊断码 |
| C6 Studio 与 Python 工具 | **推迟** |
| C7 配置与测试迁移 | 按第 7 节，配置先于测试 |
| C8 文档 | 第 12 节中的正文更新；`tools/pipeline_studio/README.md`、架构图**推迟** |

- **测试代码**：节点设计第 11 节中"工具"一行以外的全部用例。
- **验收**：除第 4 节外，节点设计第 10 节"阶段 2"中除工具以外的验收项都要满足。

## 7. 第 9 步：工具与可视化（待办）

第 1–8 步推迟的内容，在集成分支上一并完成：

| 项目 | 设计依据 |
| --- | --- |
| Catalog JSON 的键名与内容 | I/O 8.2；模型 13.1；节点第 9 节 |
| `alg_pipeline_tool` 各命令：`catalog`、`init`、`edit`、`describe-*`、`validate-io`、`resolve-conf`、`export-schema` 与诊断路径输出；`alg_show` | I/O 8.3；模型 13.2；节点 4.5、第 9 节 |
| Pipeline Studio：服务端与网页 | I/O 8.4；模型 13.3；节点第 9 节 |
| `tools/verify_selection.py`、`tools/dev_recipe.py`；资源清单中的选型模板；效果规格 `tests/fixtures/effects/` | I/O 8.5；模型 13.4；节点第 9 节 |
| 脚手架的其余功能 | 节点 3.2、5.3 |
| `dev_support/node_authoring/benchmark/run.py`；`dev_support/benchmarks/control_snapshots.*` | I/O 第 10 节"阶段 0"；节点 6.4 |
| `.github/workflows/ci.yml`：缓存路径与缓存键、kite 任务的模型路径和读取 Catalog 的脚本 | 模型第 12 节；Catalog 键名变化 |
| 架构图：`doc/architecture_classes.puml`、`architecture_flow.puml`，以及用 `./scripts/render_architecture_diagrams.sh --generate` 重新生成的 SVG | 三份设计的文档章节 |
| 工具文档：`tools/pipeline_studio/README.md`；skills 中讲命令行和 Studio 用法的部分 | 同上 |
| tier4 测试：`tests/tooling/` 中的 Python 和浏览器测试、`NativeCli_*`、`PythonCli_*`、`PipelineTool*Test`、`CustomNodeScaffoldTest`、`DevRecipeTest`、`PipelineStudioServerTest`、`DiagramAssetsCheckTest` | 三份设计测试计划中的"工具"一行 |

完成以后的收尾：
1. 运行完整门禁 `./scripts/run_all_tests.sh`；
2. 验证 kite（本地 kite 构建，或在 PR 中由 CI 验证）；本地有真实权重时运行 e2e；
3. 按三份设计的 `git grep` 清单在全仓库检查，包括 `tools/`、`src/cli/`、`tests/tooling/`；
4. 删除本文和三份设计，把形成的规则写进现行指南（各设计的文档章节已列出位置）；
5. 集成分支提 PR 合入 main。

## 8. 风险

| 项目 | 处理 |
| --- | --- |
| 集成分支持续时间长，与 main 分叉 | 在步骤边界合并 main；期间尽量不在 main 上做与这三份设计重叠的改动 |
| 第 9 步之前，Studio 和大部分命令行功能不可用 | 期间直接编辑方案 JSON，用 `validate`、`plan` 检查 |
| kite 构建依赖私有发布 | 本地没有条件时，在最后的 PR 中由 CI 验证 |
| `plans/` 中其他进行中的计划 | 业务开发者体验 RFC 只剩 WI-6（CODEOWNERS）和 O-2（CONTRIBUTING 改中文），与本轮改动的文件交集小；它的 O-1 已决定：等于默认值的参数一律删除（模型设计附录 A、节点设计附录 C） |
| 每步只跑 tier1–3，问题可能留到第 9 步才暴露 | tier4 主要覆盖工具；生产行为由黄金测试、Demo smoke 和集成测试保证 |
