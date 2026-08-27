# RFC 0010: 全栈统一业务命名为 biz (Business to Biz Architecture & Identifier Unification)

- **RFC 编号**：0010-business-to-biz-naming-unification
- **创建日期**：2026-08-27
- **文档状态**：Completed
- **关联分支**：`feat/biz-naming-unification`
- **目标版本**：v3.1.0
- **负责人 / 作者**：LLM-EdgeFlow Architecture & Quality Team

---

## 1. 背景与动机 (Motivation & Context)

### 1.1 当前痛点
在 `LLM-EdgeFlow` 早期演进过程中，“业务”这一概念在代码中存在多种混合命名：
1. **长度冗长与拼写手误风险**：`business` 单词较长，在不同开发者协作或手写配置时容易误打（如 `buisness`、`bussiness`），造成无谓的调试成本。
2. **术语命名割裂**：
   - C ABI 枚举与核心参数早已缩写为 `CompanyAlgBizType`、`ALG_BIZ_TYPE_*`、`biz_type`、`biz_name`；
   - 内部抽象类和平台适配器使用 `IBusinessAdapter`、`BusinessAdapterRegistry`、`PlatformBusinessSlot`；
   - 目录与配置使用 `src/business/`、`demo/businesses/`、`"business_name"`、`def.category = "business"`；
   - 命令行工具与检索函数使用 `--business`、`FindBusiness`。
3. 这种全称与缩写混杂的现状增加了新算子开发、跨层代码阅读及自动化工具链调用的心智负担。

### 1.2 预期收益
- 全库四层架构、目录结构、接口定义、Pipeline 配置、CLI 工具链及测试套件统一收敛为 **`biz`**。
- 彻底杜绝拼写错误隐患，代码与配置更加短小紧凑，符合极简高效原则。

---

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)
- **目录结构重构**：
  - `src/business/` -> `src/biz/`
  - `src/adapter/platform/business_bridges/` -> `src/adapter/platform/biz_bridges/`
  - `demo/businesses/` -> `demo/biz/`
- **Layer 1 (C ABI & Platform Adapter)**：
  - `IBusinessAdapter` -> `IBizAdapter`（头文件 `biz_adapter_interface.h`）
  - `BusinessAdapterRegistry` -> `BizAdapterRegistry`（头文件 `biz_adapter_registry.h`）
  - `BusinessDtoTraits` -> `BizDtoTraits`（头文件 `biz_dto_traits.h`）
  - `PlatformBusinessSlot` -> `PlatformBizSlot`
  - `PlatformBusinessBridgeDescriptor` -> `PlatformBizBridgeDescriptor`
  - `PlatformBusinessBridgeRegistry` -> `PlatformBizBridgeRegistry`（头文件 `platform_biz_bridge_registry.h`）
- **Layer 2 (Pipeline, Validator & Catalog)**：
  - `NodeDefinition.category`：统一为 `"biz"`（原 `"business"`）
  - `NodeDefinition.biz_names`（原 `business_names`）
  - `PipelineCatalog::FindBiz`（原 `FindBusiness`）
  - `PipelineConfig.biz_name`（原 `business_name`）
  - Pipeline JSON Schema 规范主键更新为 `"biz_name"`（兼容读取旧 `"business_name"`）
- **Layer 3 (Biz Nodes)**：
  - 迁移所有业务节点至 `src/biz/*`，节点元数据统一配置 `def.category = "biz"`
- **Tooling & CLI**：
  - `alg_pipeline_tool`、`alg_show`、`show` 工具统一支持 `--biz` / `"biz_name"`
- **测试与文档**：
  - 迁移更新测试用例 `test_all_biz_pipelines.cpp`、`test_platform_biz_bridge_registry.cpp` 等
  - 更新四层隔离检查脚本 `check_layer_isolation.sh` 及架构文档

### 2.2 非目标 (Non-Goals)
- 保持已有的 C ABI 导出的 6 个纯 C 符号（`Alg_Init`, `Alg_Create`, ...）名称与函数签名不变。
- 保持现有的 `CompanyAlgBizType` 枚举数值映射不变。

---

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

```text
Layer 1: C ABI & Platform Adapter
  ├── biz_adapter_interface.h (IBizAdapter)
  ├── biz_adapter_registry.h (BizAdapterRegistry)
  ├── platform_biz_bridge_registry.h (PlatformBizBridgeRegistry)
  └── biz_bridges/ (*_bridge.cpp)
      │  ▲
      ▼  │
Layer 2: Pipeline & Dynamic Blackboard
  ├── NodeDefinition { category = "biz", biz_names = [...] }
  ├── PipelineConfig { biz_name = "..." }
  └── PipelineCatalog { FindBiz(...) }
      │
      ▼
Layer 3: Pluggable Biz & Common Nodes
  ├── src/biz/ (doc_qa, keyword_match, entity_extract, dialogue_audit, ocr_doc_qa, audio_asr, cross_rerank)
  └── src/common_nodes/ (llm_generate_node)
      │
      ▼
Layer 4: Engines & Hardware Acceleration
  └── FixedBatchExecutor / IModelEngine (unchanged)
```

---

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **兼容性容错（Fail-Safe Backward Compatibility）**：
   - `PipelineConfig` 解析和 `PipelineValidator` 校验时，优先提取 `"biz_name"`，若未找到则回退兼容读取 `"business_name"`，确保外部已有配置平滑迁移。
   - `alg_pipeline_tool` CLI 优先支持 `--biz`，同时保留 `--business` 作为兼容别名。
2. **分层防腐隔离约束**：
   - 调整 `scripts/check_layer_isolation.sh`，扫描目标路径从 `src/business/` 更新为 `src/biz/`，规则继续严格生效。

---

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [ ] `tests/test_all_biz_pipelines.cpp` 编译并通过；
- [ ] `tests/test_platform_biz_bridge_registry.cpp` 编译并通过；
- [ ] `scripts/check_layer_isolation.sh` 100% 通过；
- [ ] `scripts/check_architecture_docs.sh` 100% 通过；
- [ ] `scripts/render_architecture_diagrams.sh --check` 100% 通过；
- [ ] `./scripts/run_all_tests.sh` 6 大阶段回归测试全部通过；
- [ ] 37/37 CTest 自动化测试用例 100% 通过。

---

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：RFC 方案定稿与分支建立**（`feat/biz-naming-unification`）
2. [ ] **阶段二：头文件、类名与适配器重构（Layer 1 & Layer 2）**
3. [ ] **阶段三：业务目录与节点元数据迁移（Layer 3 `src/biz/`）**
4. [ ] **阶段四：Demo、CLI 工具与 Pipeline JSON 配置同步重构**
5. [ ] **阶段五：测试用例、CI 脚本与架构文档更新**
6. [ ] **阶段六：全量构建、100% CTest 与 6 阶段测试门禁验证**
7. [ ] **阶段七：RFC 标记 Completed 并合入 `main`**

---

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :---: | :--- | :--- |
| 2026-08-27 | v3.1.0 | 创建 RFC-0010：全栈统一业务命名为 `biz` | LLM-EdgeFlow Architecture Team |
