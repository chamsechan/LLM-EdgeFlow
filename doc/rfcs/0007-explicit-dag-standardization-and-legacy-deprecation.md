# RFC 0007: 全库 Pipeline 配置文件显式 DAG 标准化与旧式配置维护解耦

- **RFC 编号**：0007-explicit-dag-standardization-and-legacy-deprecation
- **创建日期**：2026-08-23
- **文档状态**：Completed
- **关联分支**：`feat/explicit-dag-standardization`
- **目标版本**：v1.6.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

- **当前痛点**：
  在之前的演进中，LLM-EdgeFlow 引入了基于 `id` 和 `depends_on` 的显式 DAG 调度与验证机制（RFC 0003 & RFC 0006），但在 `configs/` 目录下的 11 个官方预置方案仍保留了仅依赖 JSON 数组先后顺序的隐式旧格式。这导致在 Web 可视化 Studio 中进行拓扑编辑时，经常弹出模态对话框 *“这是旧式顺序配置。首次结构编辑需要由 C++ normalize 升级为显式 DAG，是否继续？”*，增加了用户的认知负担与多版本配置的维护成本。
- **业务诉求**：
  全面将官方预置管线配置统一升级为显式 DAG 格式（`id` + `depends_on`），消除格式割裂与日常旧格式维护负担，使 Web Studio、CLI 工具与核心引擎共享一致的标准事实源。
- **预期收益**：
  1. 官方 11 个配置文件全部显式化，结构清晰、拓扑一目了然，与底层 Kahn 拓扑排序及 Wavefront 并行调度契合。
  2. Web Studio 彻底告别阻断性确认弹窗，实现开箱即用的丝滑拖拽与连线体验。
  3. 保留底层 C++ 对意外遗留配置的自动容错补齐机制，确保对外 C ABI 与系统调用的向下兼容与高鲁棒性。

---

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)
- [x] 将 `configs/` 下全部 11 个官方 Pipeline 配置文件通过官方 C++ normalize 升级为显式 DAG 标准格式。
- [x] 优化 Web Studio (`tools/visualizer/app.js`) 逻辑：所有官方方案开箱即为显式 DAG；对外部导入的隐式旧配置自动平滑标准化，移除阻塞性 confirm 弹窗。
- [x] 验证与更新测试用例（`test_pipeline_studio.cpp`、`test_pipeline_config.cpp`、`test_visualizer_server.py` 等）。
- [x] 确保全量 23 项 Google Test 单元测试与 6 阶段自动化测试套件（`./scripts/run_all_tests.sh`）100% 通过。

### 2.2 非目标 (Non-Goals / Out-of-Scope)
- 不修改 6 个公开 C ABI 函数契约。

---

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1 (C ABI / Platform Adapter)**：保持不变，对外暴露的 C ABI 与 Adapter 数据流完全不受影响。
- **Layer 2 (Pipeline & Dynamic Blackboard)**：`PipelineConfig` 与 `PipelineValidator` 彻底剔除旧式隐式顺序配置的容错分支与 bypass 逻辑，强制要求所有节点显式提供 `id` 与 `depends_on`，统一使用 Kahn 拓扑排序与 Wavefront 调度。
- **Layer 3 (Business & Common Nodes)**：所有 7 大业务共 11 个官方方案在 `configs/` 中显式标明 `id` 和 `depends_on` 依赖关系。
- **Layer 4 (Engines & Hardware Acceleration)**：保持不变。

### 3.2 配置文件标准结构 (Pipeline JSON Invariant)

所有 `configs/pipeline_*.json` 统一遵循标准 DAG 结构：

```json
{
  "business_name": "example_business_v1",
  "models": [ ... ],
  "pipeline": [
    {
      "id": "node_0_PreNode",
      "node_type": "PreNode",
      "depends_on": [],
      "config": { ... }
    },
    {
      "id": "node_1_ModelNode",
      "node_type": "ModelNode",
      "depends_on": ["node_0_PreNode"],
      "config": { ... }
    },
    {
      "id": "node_2_PostNode",
      "node_type": "PostNode",
      "depends_on": ["node_1_ModelNode"],
      "config": { ... }
    }
  ]
}
```

---

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **确定性 ID 与依赖命名规则**：
   - 升级采用 C++ 核心规范 `node_<index>_<NodeType>` 作为默认稳定 ID，首节点 `depends_on: []`，后续节点 `depends_on: [前序节点id]`。
2. **彻底消除历史技术债务与双轨维护**：
   - C++ `ParsePipelineConfig` 严格要求每个节点必须具备非空字符串 `id` 和数组 `depends_on`，缺少则直接通过 `PipelineErrorCode::kMissingField` 拦截 fail-closed。
   - `Pipeline::ResolveDagTopologicalSort` 移除旧式 bypass 逻辑，全量管线统一通过 DAG 拓扑排序执行。
3. **Web Studio 零多余分支**：
   - Web Studio 简化节点与连线交互，开箱即为标准 DAG，移除所有旧式检查与阻塞提示。

---

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] **官方配置校验测试**：全库 11 个配置文件经 `alg_pipeline_tool validate` 100% 静态校验通过。
- [x] **CLI 工具链双模验证**：`./show` (Python) 与 `./build/alg_show` (C++) 对全量配置解析渲染 100% 正常。
- [x] **Web Studio 服务验证**：`tests/test_visualizer_server.py` 自动化测试 100% 通过。
- [x] **单元与端到端回归测试**：23 项 CTest 全量通过，`./scripts/run_all_tests.sh` 6 阶段测试门禁 100% 通过。

---

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：创建特性分支与 RFC 文档**（`feat/explicit-dag-standardization`，`doc/rfcs/0007-explicit-dag-standardization-and-legacy-deprecation.md`）。
2. [x] **阶段二：执行全库 11 个配置文件的显式 DAG 规范化升级**。
3. [x] **阶段三：彻底移除 C++ / Web Studio 遗留顺序兼容分支，严格校验显式 DAG**。
4. [x] **阶段四：全量测试套件、CLI 与 6 阶段质量门禁回归验证**。
5. [x] **阶段五：更新 RFC 状态为 Completed，提交 PR 并合并至 `main`**。

---

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-23 | v0.1 | 初始设计，定义全库 11 个配置显式 DAG 升级方案与 Web Studio 平滑交互规范 | LLM-EdgeFlow Team |
| 2026-08-23 | v1.0 | 彻底移除旧式顺序兼容代码与双轨解析分支，全库强制统一为显式 DAG 标准 | LLM-EdgeFlow Team |
