# 平台与调度参数归位：实施与验收

> 本文件是阶段 5 的工作计划，不是现行规则。全部步骤验收后，把 2.3 的判定原则写入
> [`CONTRIBUTING.md`](../CONTRIBUTING.md#3-design-and-current-contracts) §3，各步骤的现行用法写入对应指南，
> 然后删除本文件（`CONTRIBUTING.md` §3、§5 不保留提案和实施报告）。
>
> 5.1–5.4 已完成（见第 4 节）。剩余内容核对于 `main@fa72e4c`，实施时以符号名为准。

## 1. 目标与步骤

业务开发者（编写 Node 函数、转换器 Decode/Encode、编排 Pipeline JSON 的人）不了解平台资源和框架调度，也能接入并运行。
平台限制和调度参数由框架给默认值或自动推导；只有负责性能验收或部署的人，在有依据时才显式覆盖。

| 步骤 | 内容 | 规模 | 设计审查 | 前置条件 | 建议分支 | 状态 |
| --- | --- | --- | --- | --- | --- | --- |
| 5.1 | Studio 保留"未配置"状态；Backend 字段收进"部署高级设置" | 小 | 不需要 | 无 | `fix/studio-unset-defaults` | 已完成（PR #150） |
| 5.2 | 接入层平台数值归位：Binding 默认批次上限、输入长度上限单一来源、输出容量字段从 ValueType 推导 | 小 | 不需要 | 阶段 3 已合入 `main` | `refactor/adapter-platform-limits` | 已完成（PR #150） |
| 5.3 | 单次有效批次可查询；删除无读取方的字段；存量默认值清单 | 小 | 不需要 | 阶段 3 已合入 `main` | `chore/effective-limits-tooling` | 已完成（PR #150） |
| 5.4 | 请求编号回传移出业务契约 | 中 | 需要 | 5.2 已合入（改动同一批转换器文件） | `refactor/adapter-request-ids` | 已完成（PR #151） |
| 5.5 | 并行层按约束自动串行 | 中 | 需要 | D1 已确认 | `feat/auto-serialize-parallel-layers` | 未开始，待 D1 确认 |

建议顺序：5.1 → 5.2 → 5.3 → 5.4；5.5 与其他步骤没有代码依赖，D1 确认后即可进行。

## 2. 约束

### 2.1 不变的外部契约与检查方法

| 契约 | 检查方法 |
| --- | --- |
| 公共 Operator 接口（`include/edgeflow/`）和返回码 | `git diff --stat main...HEAD -- include/edgeflow include/platform_mock` 无输出；`CppOperatorSdkTest`、`OperatorSafetyTest` 通过 |
| 宿主结构体、槽位名、`request_id` 字段、`COMPANY_OPERATOR_MAX_RERANK_CANDIDATES` | 同上 |
| `.conf` 格式、Pipeline JSON 格式和现有字段语义 | 基线对比中 `pipeline-schema.json` 一致；`git diff --stat main...HEAD -- configs demo/fixtures demo/profiles.json` 无输出 |
| 输出池语义和容量 | 基线对比中 `resolve-conf` 的 `output_pools` 一致 |
| Demo 与 SDK 的交互 | Demo 只调用公共 Operator 接口；smoke 结果与基线一致 |
| 已发布的 Control 命令 ID | 基线对比中 Catalog 的 `nodes` 一致 |
| 输入的接受/拒绝边界、检查顺序和返回码 | `AdapterContractSecurityTest` 的 `InputLengthLimitsStayUnchanged`、`OperatorInputLimitsStayUnchanged` |
| 各业务的有效批次上限（64） | 基线对比中 `validate-io` 的 `effective_max_batch_size` 一致 |

唯一有意改变的外部行为是 5.5：原本 Create 返回 `-2` 的并行配置可以运行（见 D1）。

下列内部输出允许变化，对比时按第 3 节的参数排除：

| 步骤 | 允许的变化 |
| --- | --- |
| 5.5 | `max_parallel_workers > 1` 的配置，`plan` 中的 `layers` 按约束拆分（仓库内现有配置都是 1，不受影响） |

### 2.2 决策点

| 编号 | 问题 | 默认做法 | 确认人 |
| --- | --- | --- | --- |
| D1 | 5.5 的兼容放宽：原本因 `NODE_NOT_PARALLEL_SAFE`、`SERIALIZED_MODEL_CONCURRENCY` 被拒绝、Create 返回 `-2` 的配置变为可运行 | 未确认前不实施 5.5 | 项目负责人 |
| D3 | rerank 候选段落的上限：Operator 层 10 MiB，转换器 64 KiB | 现状：转换器保持 64 KiB，具名常量 `kMaxCandidatePassageBytes`（`src/adapter/input/rerank_input.cpp`），取值待确认 | 方案负责人 |
| D4 | 配置中显式写出、且等于默认值的字段是否删除 | 现状：只出清单（附录 A.3 的脚本），未改配置 | 方案负责人 |

### 2.3 判定原则（阶段完成后写入 CONTRIBUTING §3）

- 存在对所有场景都正确的保守值（最多慢一些），就由框架给默认值。
- 运行时另有硬上限兜底时，也可以给默认值。
- 语义契约（端口、类型、数量关系、执行协议、外部协议 ID）不给默认值，靠清晰的报错引导。
- 默认值写在 Definition 或注册表上，Catalog 和工具可见；工具不得把未修改的默认值写成显式配置。
- 覆盖途径：组件能力上限写在代码里；按部署调整写在 Pipeline JSON 的现有字段里；`.conf` 由平台定义，不加字段。

## 3. 每个步骤的通用流程

1. 从最新 `main` 建分支。需要设计审查的步骤，先在 PR 描述中写清问题、方案、受影响的契约、取舍、验收标准和回退方式。
2. 构建并采集基线（脚本见附录 A，保存到已被 Git 忽略的 `build/param-baseline/`）：

   ```bash
   cmake --build build -j 4
   build/param-baseline/capture_baseline.sh before
   ```

3. 先写本步骤"测试"中锁定现有行为的用例，确认它们在未改动的代码上通过；标明"改动前失败"的用例除外。
4. 实施改动，运行本步骤的聚焦测试。
5. 更新本步骤列出的文档；用户可感知的变化写进 `doc/CHANGELOG.md` 的 Unreleased。
6. 运行门禁 `./scripts/run_all_tests.sh`。
7. 采集改动后的结果并对比：

   ```bash
   build/param-baseline/capture_baseline.sh after-5.x
   python3 build/param-baseline/compare_baseline.py \
     build/param-baseline/before build/param-baseline/after-5.x <本步骤的对比参数>
   ```

8. 逐项勾选验收清单，把命令和结果记录在 PR 中。

## 4. 已完成的步骤（5.1–5.4）

设计、验收命令与结果在对应 PR 中；现行用法写在各自的指南里，本文件不再重复。

| 步骤 | PR | 现行用法 | 未决事项 |
| --- | --- | --- | --- |
| 5.1 Studio 保留"未配置"状态 | #150 | `tools/pipeline_studio/README.md`；回归测试 `tests/tooling/studio_config_roundtrip_test.mjs` | 无 |
| 5.2 接入层平台数值归位 | #150 | `doc/dev_guide/business_onboarding.md`、`src/adapter/biz/README.md`、`src/adapter/input/README.md` | D3 |
| 5.3 单次有效批次可查询与清理 | #150 | `doc/dev_guide/business_onboarding.md` 第 6 节、`tools/pipeline_studio/README.md` | D4 |
| 5.4 请求编号回传移出业务契约 | #151（设计审查见 PR 描述） | `doc/dev_guide/business_onboarding.md`；迁移说明见 `doc/CHANGELOG.md` | 无 |

## 5. 步骤 5.5：并行层按约束自动串行

**前提**：D1 已确认。在 `feat/auto-serialize-parallel-layers` 上实施，一个提交即可。本节核对于 `main@12b3345`，行号以符号名为准。

**问题**：`max_parallel_workers > 1` 时，Validator 拒绝含非 `parallel_safe` 节点的并行层，也拒绝共享串行模型的并行层
（`src/core/pipeline_validator.cpp:1490-1537` 的 `if (parsed.max_parallel_workers > 1)` 块），Create 因此返回 `-2`。
运行时对串行模型不加锁，这个拒绝是唯一保护。执行器对单节点层已在主线程顺序执行（`src/core/pipeline.cpp:419-432`），
所以只改 Validator：把这些节点拆到单独的层，就能在不加锁的前提下安全执行。

**外部契约**：Operator 接口、返回码集合、宿主结构体、`.conf`、Pipeline JSON 格式、`plan` / `validate` 输出的 JSON 结构、
诊断码表（两个诊断码保留，只是不再产生）都不变。唯一的行为变化是 D1：原本被这两个诊断拒绝的配置可以运行。
仓库内的配置都没有设置 `max_parallel_workers`（默认 1），不受影响。

### 规则

只处理 `max_parallel_workers > 1` 的计划，逐层进行：

1. 按层内原顺序遍历节点。节点**不是** `parallel_safe`，放入"顺序组"。
2. 否则取它绑定的串行模型集合（`model_bindings` 中 `model_id` 非空、且 `model_concurrency` 为 `kSerialized` 的 `model_id`，去重；与原检查相同）。
   与"并行组"已占用的串行模型有交集，放入"顺序组"；没有交集，放入"并行组"并占用这些模型。
3. 输出：并行组非空时作为一层（保持原顺序），之后顺序组的每个节点各成一层（保持原顺序）。
4. 全部层处理完后，用新层替换 `report.topological_layers`，并把 `report.topological_order` 重建为新层依次展开的结果，
   使两者一致（同层节点之间没有依赖，任何顺序都是合法的拓扑序）。

推论：全部节点都安全、且不共享串行模型的层保持原样；单节点层不变；`max_parallel_workers == 1` 的计划完全不变。
同一层内写同一端口的 `kParallelWriteConflict` 仍按**原始层**检查并拒绝。

执行器不需要改：`Pipeline::BuildFromPlan` 按 `report.topological_layers` 实例化节点（`pipeline.cpp:133`），执行（`:419` 起）和
`Control` 分发（`:564` 起）也按同一份层遍历，三者始终一致。拆出的单节点层走现有的主线程分支。

### 改动

1. `src/core/pipeline_validator.cpp`，把 `if (parsed.max_parallel_workers > 1)` 整个块替换为下面的写法（写冲突检查的代码原样保留）：

   ```cpp
   if (parsed.max_parallel_workers > 1) {
     // Nodes that must not share a wavefront run in their own layers instead of
     // rejecting the plan. Write conflicts are still checked per original layer.
     auto serialized_models = [&](const std::string& id) {
       std::unordered_set<std::string> models;
       for (const auto& binding : plan.node_plans[id].model_bindings) {
         if (binding.model_id.empty()) continue;
         auto it = model_concurrency.find(binding.model_id);
         if (it != model_concurrency.end() &&
             it->second == InferenceConcurrency::kSerialized) {
           models.insert(binding.model_id);
         }
       }
       return models;
     };
     std::vector<std::vector<std::string>> scheduled;
     for (const auto& layer : report.topological_layers) {
       std::unordered_map<std::string, std::string> writes;
       std::vector<std::string> parallel;
       std::vector<std::string> sequential;
       std::unordered_set<std::string> used_models;
       for (const auto& id : layer) {
         auto def_it = def_by_id.find(id);
         if (def_it == def_by_id.end() || !def_it->second->parallel_safe) {
           sequential.push_back(id);
         } else {
           auto models = serialized_models(id);
           const bool shares = std::any_of(
               models.begin(), models.end(),
               [&](const std::string& m) { return used_models.count(m) > 0; });
           if (shares) {
             sequential.push_back(id);
           } else {
             used_models.insert(models.begin(), models.end());
             parallel.push_back(id);
           }
         }
         if (def_it == def_by_id.end()) continue;
         // 原有的 kParallelWriteConflict 检查（遍历 plan.node_plans[id].ports）原样放在这里。
       }
       if (!parallel.empty()) scheduled.push_back(std::move(parallel));
       for (auto& id : sequential) scheduled.push_back({std::move(id)});
     }
     report.topological_layers = std::move(scheduled);
     report.topological_order.clear();
     for (const auto& layer : report.topological_layers) {
       report.topological_order.insert(report.topological_order.end(),
                                       layer.begin(), layer.end());
     }
   }
   ```

   原块中产生 `kNodeNotParallelSafe` 和 `kSerializedModelConcurrency` 的代码全部删除。确认文件已包含 `<algorithm>` 和 `<unordered_set>`。
2. `include/core/diagnostic_code.h`：两个诊断码及名称保留（码表不变），在这两行旁加注释
   `// Retained for compatibility; the Validator now schedules such nodes in separate layers.`
3. 其他文件不改。

### 测试

都只改现有用例或在现有文件中新增，不新建测试文件。

| 文件 / 用例 | 改为 |
| --- | --- |
| `tests/unit/core/test_validated_pipeline_plan.cpp` 的 `RejectsSharedSerializedModelInParallelLayer` | 改名为 `SplitsSharedSerializedModelIntoSequentialLayers`。`max_parallel_workers = 4` 时断言 `report.ok`，层为 `[["node_a"], ["node_b"]]`，`topological_order` 为 `["node_a", "node_b"]`；改为 1 时层为 `[["node_a", "node_b"]]`；保留"加 `depends_on` 后通过"的断言 |
| 同文件 `MultiModelBindingsAndConcurrencyDeduplication` | 第 1 段补充断言层为 `[["multi_node", "parallel_node"]]`（独立实例仍并行）；第 2 段把"被拒绝"改为断言 `report.ok`，层为 `[["multi_node"], ["parallel_node"]]` |
| 同文件 `WorkerBudgetControlsParallelSafetyChecks` | 改名为 `WorkerBudgetSchedulesUnsafeNodesSequentially`。`max_parallel_workers = 2` 时断言 `report.ok`，层为 `[["independent"], ["source"]]`（`PlanTestNode` 安全、`FlowContractProducerNode` 不安全），`topological_order` 为 `["independent", "source"]`；为 1 时层为 `[["source", "independent"]]` |
| `tests/fixtures/pipelines/validation/invalid_pipeline_cases.json` | 删除 `serialized_model_concurrency` 用例（两个读取方按用例循环，不依赖用例数）；`parallel_write_conflict` 用例保留 |
| `tests/integration/pipeline/test_pipeline_catalog_validator.cpp` | 新增 `PipelineValidatorTest.SerializedModelBranchesRunInSeparateLayers`：用被删除用例的 `pipeline`，断言 `report.ok`，层为 `[["pre"], ["left"], ["right"], ["post"]]` |
| `tests/unit/core/test_dag_pipeline.cpp` | 新增不安全节点 `DagTestUnsafeNodeC`：继承 `DagTestNodeC`，另设 `kNodeType = "DagTestUnsafeNodeC"` 和 `Name()`；新增 `inline NodeDefinition MakeUnsafeDagNodeDef()`，调用 `MakeDagNodeDef(DagTestUnsafeNodeC::kNodeType, {{"node_a_out", "string"}}, {{"node_c_out", "string"}})` 后把 `parallel_safe` 置为 `false`，用 `REGISTER_NODE_WITH_DEFINITION(DagTestUnsafeNodeC, MakeUnsafeDagNodeDef())` 注册。新增 `UnsafeNodeRunsInOwnLayer`：复用 `ParallelWavefrontExecution` 的菱形配置，把 `node_c` 换成该类型；`max_parallel_workers` 为 4 时层为 `[["node_a"], ["node_b"], ["node_c"], ["node_d"]]`，`Execute` 返回 0，`final_dag_result` 与原用例相同；为 1 时结果相同 |

`ParallelWavefrontExecution`、`test_engine_fault_tolerance_and_lifecycle.cpp` 中的深层 DAG 用例都只用安全节点、不共享串行模型，层不变，不用改。

### 文档与 CHANGELOG

- `doc/dev_guide/custom_node_concepts.md:188-190`：把"若该节点处于含多个节点的并行层，Validator 会拒绝这个计划；它不会自动加锁，也不会自动把那一层改成串行"
  改为"框架把它放到单独的层顺序执行，不会自动加锁；声明 `.ParallelSafe(true)` 后才可能与同层节点并行。共享串行模型的节点也按同样方式分层"。
- 其他提到并发的文档（`configs/README.md:46`、`doc/architecture.md:184`、`doc/developer_guide.md:110`、`src/custom_nodes/README.md:37`
  以及 `.agents` 下三处）只说"大于 1 时启用并行调度和安全检查"或"确认安全后再设为 `true`"，改动后仍然成立，不用改。
- CHANGELOG：`max_parallel_workers > 1` 时，Validator 不再以 `NODE_NOT_PARALLEL_SAFE`、`SERIALIZED_MODEL_CONCURRENCY` 拒绝计划，
  而是把未声明并行安全的节点、以及会共享串行模型的节点拆到单独的层顺序执行；原本 Create 返回 `-2` 的这类配置现在可以运行。
  两个诊断码保留但不再产生；`plan` 输出中的 `layers` 与 `topological_order` 反映拆分后的执行顺序。

### 验收

- [ ] 基线对比（附录 A，改动前在 `main` 上采集，无额外参数）0 个差异：仓库内配置都是默认 `max_parallel_workers = 1`。
- [ ] `git grep -n "kNodeNotParallelSafe\|kSerializedModelConcurrency" -- src` 无输出（两个码只在 `include/core/diagnostic_code.h` 中定义）。
- [ ] 聚焦测试通过：`ctest --test-dir build --output-on-failure -R "ValidatedPipelinePlanTest|DagPipelineTest|PipelineStudioTest|PipelineStudioServerTest"`
  （`PipelineValidatorTest.*` 在 `PipelineStudioTest` 中运行，fixture 循环在 `PipelineStudioServerTest` 中运行）。
- [ ] 2.1 的外部契约检查通过（只有 D1 这一项行为变化）；门禁通过。

**回退**：还原本步骤的提交；不涉及配置迁移。

## 6. 阶段完成

- [x] 5.1–5.4 全部验收（PR #150、#151）。
- [ ] D1 确认后 5.5 已验收；未确认时在 PR 中注明未实施。
- [ ] 2.3 的判定原则写入 `CONTRIBUTING.md` §3。
- [ ] `FRAMEWORK_SIMPLIFICATION_PLAN.md` 标记阶段 5 完成。
- [ ] 删除本文件，并清理指向本文件的引用。

## 附录 A：基线与报告脚本

把以下三个文件保存到 `build/param-baseline/`（`build/` 已被 Git 忽略）并加上执行权限，从仓库根目录运行。
三个脚本都已在基线上运行过：连续采集两次，对比结果为 0 个差异。

### A.1 `capture_baseline.sh`

采集 Catalog、Pipeline Schema、26 份配置的 `validate-io`、`resolve-conf`（7 种池深）和 `plan`，以及 smoke 结果。
`configs/` 用 `alg_pipeline_tool`，`demo/fixtures/mock/` 用 `alg_pipeline_tool_test`。
kite、whisper 配置在默认构建中会失败，对比时只要求失败结果一致。

```bash
#!/usr/bin/env bash
# 用法：capture_baseline.sh <标签>      例如 before、after-5.2
# 从仓库根目录运行；结果写入 ${BASELINE_ROOT:-build/param-baseline}/<标签>/
set -uo pipefail
label="${1:?需要标签，例如 before}"
out="${BASELINE_ROOT:-build/param-baseline}/${label}"
rm -rf "${out}" && mkdir -p "${out}/validate-io" "${out}/resolve-conf" "${out}/plan"
tool=./build/alg_pipeline_tool
test_tool=./build/alg_pipeline_tool_test
"${tool}" catalog > "${out}/catalog.json" 2>&1
"${test_tool}" catalog > "${out}/catalog-test.json" 2>&1
"${tool}" export-schema > "${out}/pipeline-schema.json" 2>&1
capture_conf() {  # <工具> <conf> <输出名>
  local runner="$1" conf="$2" name="$3" depth
  "${runner}" validate-io "${conf}" --model-root . > "${out}/validate-io/${name}.json" 2>&1
  for depth in 0 1 25 64 100 1024 1025; do
    "${runner}" resolve-conf "${conf}" --root . --depth "${depth}" \
      > "${out}/resolve-conf/${name}.depth${depth}.json" 2>&1
  done
}
for conf in configs/*.conf; do capture_conf "${tool}" "${conf}" "$(basename "${conf}" .conf)"; done
for pipe in configs/*.json; do "${tool}" plan "${pipe}" > "${out}/plan/$(basename "${pipe}" .json).json" 2>&1; done
for pipe in demo/fixtures/mock/*.json; do "${test_tool}" plan "${pipe}" > "${out}/plan/mock_$(basename "${pipe}" .json).json" 2>&1; done
for conf in demo/fixtures/mock/*.conf; do capture_conf "${test_tool}" "${conf}" "mock_$(basename "${conf}" .conf)"; done
./build/alg_demo --suite smoke --output-dir "${out}/smoke" > "${out}/smoke.log" 2>&1
echo "smoke exit=$?" >> "${out}/smoke.log"
echo "baseline written to ${out}"
```

### A.2 `compare_baseline.py`

逐文件比较两次采集结果。默认忽略 smoke 的耗时字段，日志只比较 smoke 退出码。
`--ignore-key` 删除指定字段，`--drop-port` 删除指定端口条目和同名映射，`--sort-list` 把指定列表按集合比较。

```python
#!/usr/bin/env python3
"""比较两次 capture_baseline.sh 的结果。

用法：python3 compare_baseline.py <基线目录> <改动后目录>
          [--ignore-key KEY ...] [--drop-port NAME ...] [--sort-list KEY ...]
默认忽略 smoke 结果中的耗时字段；日志文件只比较末尾的 smoke 退出码。
"""
import argparse
import json
import pathlib
import sys

LATENCY_KEYS = {"latency_ms", "avg_latency_ms", "total_latency_ms"}


def normalize(value, ignore, drop_ports, sort_lists):
    if isinstance(value, dict):
        result = {}
        for key, item in value.items():
            if key in ignore or key in drop_ports:
                continue
            item = normalize(item, ignore, drop_ports, sort_lists)
            if key in sort_lists and isinstance(item, list):
                item = sorted(item, key=lambda x: json.dumps(x, sort_keys=True))
            result[key] = item
        return result
    if isinstance(value, list):
        return [normalize(item, ignore, drop_ports, sort_lists) for item in value
                if not (isinstance(item, dict) and item.get("key") in drop_ports)
                and not (isinstance(item, str) and item in drop_ports)]
    return value


def load(path, options):
    text = path.read_text(encoding="utf-8", errors="replace")
    if path.suffix == ".log":
        return [line for line in text.splitlines() if line.startswith("smoke exit=")]
    try:
        if path.suffix == ".jsonl":
            data = [json.loads(line) for line in text.splitlines() if line.strip()]
        else:
            data = json.loads(text)
    except json.JSONDecodeError:
        return text
    return normalize(data, LATENCY_KEYS | set(options.ignore_key),
                     set(options.drop_port), set(options.sort_list))


def first_difference(a, b, path=""):
    if type(a) is not type(b):
        return path or "/"
    if isinstance(a, dict):
        for key in sorted(set(a) | set(b)):
            if key not in a or key not in b:
                return f"{path}/{key}"
            found = first_difference(a[key], b[key], f"{path}/{key}")
            if found:
                return found
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path} (length {len(a)} != {len(b)})"
        for index, (x, y) in enumerate(zip(a, b)):
            found = first_difference(x, y, f"{path}/{index}")
            if found:
                return found
        return None
    return None if a == b else (path or "/")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("before")
    parser.add_argument("after")
    parser.add_argument("--ignore-key", action="append", default=[])
    parser.add_argument("--drop-port", action="append", default=[])
    parser.add_argument("--sort-list", action="append", default=[])
    options = parser.parse_args()
    before, after = pathlib.Path(options.before), pathlib.Path(options.after)
    files = sorted({p.relative_to(before) for p in before.rglob("*") if p.is_file()} |
                   {p.relative_to(after) for p in after.rglob("*") if p.is_file()})
    failures = 0
    for relative in files:
        a, b = before / relative, after / relative
        if not a.exists() or not b.exists():
            print(f"MISSING {relative} ({'before' if not a.exists() else 'after'})")
            failures += 1
            continue
        left, right = load(a, options), load(b, options)
        if left != right:
            print(f"DIFF {relative}: {first_difference(left, right)}")
            failures += 1
    print(f"{len(files)} files compared, {failures} difference(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
```

### A.3 `report_explicit_defaults.py`

只读，列出配置中"显式写出、且等于注册默认值"的字段，输出 Markdown 表格交方案负责人（5.3、D4）：

```bash
./build/alg_pipeline_tool_test catalog > build/param-baseline/catalog-test.json
python3 build/param-baseline/report_explicit_defaults.py build/param-baseline/catalog-test.json \
  configs/pipeline_*.json demo/fixtures/mock/pipeline_*.json > build/param-baseline/explicit-defaults.md
```

```python
#!/usr/bin/env python3
"""列出 Pipeline 配置中"显式写出且等于注册默认值"的字段，供方案负责人逐项确认。

用法：python3 report_explicit_defaults.py <catalog.json> <pipeline.json>...
只读，不修改任何配置；当前构建未编入的 Node/Model/Backend 标记为"无法核对"。
"""
import json
import sys


def defaults(catalog, kind, key):
    items = catalog.get(kind, [])
    items = items if isinstance(items, list) else list(items.values())
    return {item[key]: {field["name"]: field.get("default")
                        for field in item.get("config_fields", [])
                        if field.get("default") is not None}
            for item in items}


def main():
    catalog = json.load(open(sys.argv[1], encoding="utf-8"))
    nodes = defaults(catalog, "nodes", "node_type")
    models = defaults(catalog, "models", "model_type")
    backends = defaults(catalog, "backends", "backend_type")
    print("| 配置 | 位置 | 字段 | 值 |")
    print("| --- | --- | --- | --- |")
    total = unknown = 0
    for path in sys.argv[2:]:
        doc = json.load(open(path, encoding="utf-8"))
        sections = [(f"pipeline[{n['id']}].config", nodes.get(n["node_type"]), n.get("config", {}))
                    for n in doc.get("pipeline", [])]
        for m in doc.get("models", []):
            sections.append((f"models[{m['model_id']}].model_config", models.get(m["model_type"]), m.get("model_config", {})))
            sections.append((f"models[{m['model_id']}].backend_config", backends.get(m["backend"]), m.get("backend_config", {})))
        for where, known, values in sections:
            if known is None:
                unknown += len(values)
                continue
            for field, value in values.items():
                if field in known and known[field] == value:
                    total += 1
                    print(f"| `{path}` | `{where}` | `{field}` | `{json.dumps(value, ensure_ascii=False)}` |")
    print(f"\n共 {total} 个字段等于注册默认值；{unknown} 个字段属于当前构建未编入的类型，无法核对。")


if __name__ == "__main__":
    main()
```

## 附录 B：现状证据

| 发现 | 证据 | 步骤 |
| --- | --- | --- |
| 去掉三份 CPU 配置中等于默认值的字段（分别 16、25、9 个）后，`resolve-conf` 的 `effective_pipeline` 逐字节一致，说明这些显式值不改变有效配置；`plan` 只输出拓扑，不能用来证明配置等价（核对于 `c894657`） | 附录 A 的工具 | D4 |
| 并行时 Validator 拒绝不安全节点和共享串行模型；运行时对串行模型不加锁；执行器已在主线程顺序执行单节点层 | `src/core/pipeline_validator.cpp:1490-1525`；`src/core/pipeline.cpp:419-432` | 5.5 |

## 附录 C：不在本阶段做的事项

| 事项 | 结论 | 理由或触发条件 |
| --- | --- | --- |
| 部署配置与业务编排分离（按硬件拆出 `models`、`backend_config`） | 不做 | 改变 Pipeline JSON 格式 |
| 合并 `model_config.max_batch_size` 与 `backend_config.max_batch_size` | 不做 | 现有配置写了这两个字段，未知字段会被拒绝 |
| 新增 SDK 接口查询有效批次 | 不做 | 改变公共 Operator 接口；改由 `resolve-conf` 查询 |
| Demo 读取内部有效限制并自动分批 | 不做 | Demo 应和真实宿主一样只通过 SDK 运行 |
| 批量删除显式等于默认值的字段；`validate` 对此报警告 | 不做 | 显式值可能是有意固定；交方案负责人（D4） |
| Control 命令 ID 辅助分配 | 再新增 custom Control 时 | 脚手架建议未占用 ID 并检查冲突，ID 仍写在源码里长期固定；工具看不到其他分支和已退役 ID，注册时的冲突检查仍是最后防线 |
| 会话缓存 key 由框架组合 | 第二个 Node 需要会话缓存时 | 共享执行、失败共享和重试已由 `GetOrCreateResult` 负责，只需补 key 组合辅助；`lifetime` 同时决定端口兼容，语义不变 |
| Model 批处理要求改为声明 | 新增下一个 Model 时 | `ModelDefinition` 声明批处理方式，工厂统一校验；两个 `max_batch_size` 字段都保留 |
