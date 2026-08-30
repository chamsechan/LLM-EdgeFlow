# RFC 0024: 正式接入前历史兼容契约清理

- **RFC 编号**：0024-pre-release-contract-cleanup
- **创建日期**：2026-08-30
- **文档状态**：Completed
- **关联分支**：`refactor/pre-release-contract-cleanup`
- **目标版本**：v7.0.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

仓库尚未正式交付，也没有外部业务基于现有 C++、JSON、CLI 或工具接口扩展，但多轮架构
迁移仍留下了若干只为旧命名或旧文档形状服务的双轨入口：Operator 值类型后缀别名、
隐式顺序 Pipeline 转换器、`.conf` 双根结构与单模型简写、Node 双配置字段，以及 Demo/C++
包装别名。这些路径没有生产消费者，却扩大注册、校验、回滚、测试和作者文档的状态空间。

正式接入前是删除这些兼容成本的最后低风险窗口。本 RFC 在不改变当前六个 C ABI 导出和
实际业务能力的前提下，规定每类契约只保留一个规范入口，旧形状直接 fail-closed。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] Operator 值类型只接受规范 suffix，删除 aliases、归一化表及对应冲突/回滚分支。
- [x] Operator `.conf` 只接受根级 `data` 对象，模型覆盖只接受 `data.model_paths` 映射。
- [x] Pipeline 从 Core、CLI、Web API、终端展示和 Skill 全链路只接受显式 `id` 与
  `depends_on`，删除隐式顺序文档 normalizer。
- [x] `TextRuleMatchNode` 只接受 `categories`；`TextTemplateNode` 只接受 `template`，
  Layer 1 负责把强类型 Operator Prompt 参数转换为该规范字段。
- [x] 结构化文档只保留 `StructuredDocumentBatch` 及显式成员访问，删除类型和字符串转换
  别名。
- [x] 删除无生产调用方的旧注册宏、Demo 包装函数、冗余长 CLI 别名和
  Studio 兼容启动器。
- [x] 消除 Node 错误码中仅为旧数值保留的跨 Node 重叠。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不修改六个 `Alg_*` C ABI、异常屏障、当前 Company DTO 布局或 Operator 函数表。
- 不删除具有当前产品语义的显式策略，例如 JSON 解析 failure policy、Demo
  `--allow-fallback-sample` 或私有扩展校验策略。
- 不改写历史 RFC 和验收报告；本 RFC 取代 RFC-0004/0007/0009 中相关兼容保留决策，
  并扩展 RFC-0012/0023 的唯一契约原则。
- 不增加新的 Node、Model、Backend 或业务行为。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1**：值类型表按 canonical suffix 精确查询；Resolver 只解析唯一 `.conf`
  Schema；Operator Control 在边界完成外部字段到 Node 规范字段的转换。
- **Layer 2**：Validator 只校验和规划显式 DAG，不再承担旧文档升级职责。
- **Layer 3**：Node Definition、初始化和 Control schema 各字段保持唯一；结构化数据不再
  暴露旧字符串视图。
- **Layer 4**：无修改。
- **Tooling / Skill**：CLI、Studio、终端展示和 `pipeline-composer` 只生成、读取和指导
  当前显式契约。

依赖方向仍为 Layer 1 → Layer 2 → Layer 3 → Layer 4。

### 3.2 唯一契约

```text
Operator key suffix: exact canonical suffix only
.conf: { "data": { "pipe_path", "model_paths", "mem_que" } }
Pipeline node: { "id", "node_type", "depends_on", "ports", "config" }
Rule categories field: categories
Template update field: template
Structured output type: StructuredDocumentBatch
```

未知 suffix、缺少 `data`、`data.model_path`、缺少 DAG 字段和旧 Node 字段均通过已有严格
边界返回错误，不进行探测、补齐或静默重写。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **有意破坏兼容**：仓库没有外部接入方，因此不设置废弃期、环境开关或转换工具。
2. **边界转换唯一**：外部 Operator 结构字段与内部 Node JSON 字段不同名时，只在
   Layer 1 转换一次，Node 不同时接受两种拼写。
3. **精确注册**：值类型 Registry 只维护 canonical map；注册事务仍以 copy-and-swap
   保证异常不污染已发布快照。
4. **严格 DAG**：所有生产配置已显式化；缺少 `id/depends_on` 是无效输入，而不是可升级
   的第二种 Schema。
5. **测试关注当前契约**：保留未知字段、非法结构和事务回滚覆盖，但不为已删除入口维护
   专用成功路径。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] Operator Registry 测试证明只解析 canonical suffix，注册异常仍原子回滚。
- [x] Resolver 测试证明缺少 `data`、`data.model_path` 和未知模型映射 fail-closed；全部
  官方 `.conf` 使用 `data.model_paths`。
- [x] Pipeline CLI/Studio 测试证明 normalizer/API 已删除，显式 DAG 的 validate/plan
  保持一致。
- [x] Node 测试证明唯一 Rule/Template 字段、结构化类型显式访问和不重叠错误码。
- [x] 受影响官方 Pipeline 完成 Catalog、Validator、plan 与 Smoke 回归。
- [x] 更新后的 `pipeline-composer` 通过 skill quick validation。
- [x] 最终运行 `./scripts/run_all_tests.sh`，85/85 CTest 通过。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：审计生产代码、工具、测试、配置与历史 RFC 中的双轨入口**。
2. [x] **阶段二：建立 RFC 并冻结唯一契约与非目标**。
3. [x] **阶段三：按 Layer 1、Layer 2、Layer 3 与 Tooling 原子迁移仓内消费者**。
4. [x] **阶段四：完成聚焦验证、Skill 校验和统一完整质量门禁**。
5. [x] **阶段五：将 RFC 标记为 Completed；远程交付仍需用户另行授权**。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-30 | v1.0.0 | 建立正式接入前唯一契约与历史兼容清理范围 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.1.0 | 完成全链路清理，Skill/架构图校验与 85 项 CTest 全部通过 | LLM-EdgeFlow Team |
