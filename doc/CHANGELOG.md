# Changelog

本文档记录 LLM-EdgeFlow 的架构里程碑与用户可感知变更。完整的设计动机、接口细节和验收证据由对应 RFC 维护；测试数量等易过时信息不在此重复记录。

仓库当前尚未发布对应 Git tag，因此以下版本号表示项目里程碑，而非可下载的正式 Release。

## Unreleased

- 引入统一 `3rdparty/` 持久化预编译归档体系，支持 llama.cpp、ONNX Runtime、GoogleTest、nlohmann/json 及 PCRE2 的本地静态库与头文件秒级复用，消除 clean build 与日常开发中的重复下载与源码重新编译耗时。

- v7 在正式接入前删除未被外部使用的历史兼容层：Operator 后缀别名、
  `.conf` 双结构/单模型简写、隐式顺序 Pipeline 转换、Node 双字段与类型别名、
  重叠错误码，以及 Demo/CLI/Studio 兼容入口。当前契约对旧形状一律 fail-closed。
- 设计依据：[RFC-0024](rfcs/0024-pre-release-contract-cleanup.md)。
- v6 只保留 `biz_name`、`--biz` 与 Biz C++ API；Catalog 升级为 schema v2，并删除
  Business 双字段、双输出和类型/方法别名。
- Node 只保留 `Init(const NodeInitContext&)`；`AlgContext` 只保留 write-once
  `Publish/Read` 请求值契约，删除覆盖快照链与 `Set/Get/Erase/Clear` 迁移接口。
- 删除不参与 Validator 决策的 `PortDefinition::allow_override`，重复生产者继续统一
  fail-closed。
- 设计依据：[RFC-0023](rfcs/0023-v6-contract-convergence.md)。
- `TextTemplateNode` 的截断策略改为只在 UTF-8 code point 边界结束，不再产生残缺的中文或 emoji 字节序列。
- `TextRuleMatchNode` 支持原子地联合更新 categories 与 rules；任一候选无效时保留完整旧配置。
- 并行波前为每个 Node 捕获独立错误诊断，首个失败节点的错误码与消息不再被同层其他失败覆盖。
- Operator 值类型 Binding 统一持有显式 I/O 方向、输出字符串容量 Schema 和池预算；
  业务 Bridge 完整性改为按已注册 Adapter 快照审计，新增业务不再维护中央 ID 列表。
- `TextRuleMatchNode` 改用 Unicode PCRE2 语义，正确支持正/负 lookbehind 与命名捕获，
  消除 `(?<` 字符串重写导致的静默规则篡改。
- `TextChunkNode` 按 Unicode code point 执行 `chunk_size` 与 `overlap`，不再截断
  UTF-8 多字节字符；非法 UTF-8 输入统一 fail-close。
- 设计依据：[RFC-0022](rfcs/0022-text-processing-safety.md)。
- 统一 Traceable 推理批次的数量与 provenance 校验；LLM、Embedding、Rerank、OCR、ASR 对无效模型输出一致 fail-close，且无效 Embedding 结果不进入 Session 缓存。
- Layer 2 Pipeline 改为局部事务式装配，Node Init 失败不再向公开 Session
  暴露部分模型或执行资源。
- Validator 与 Blackboard 统一为 write-once 输出语义，显式 DAG 文档按严格
  object 与字段契约 fail-closed。
- 并行波前在节点异常时会等待同层已提交任务全部结束，再返回稳定错误。
- 设计依据：[RFC-0020](rfcs/0020-layer2-runtime-convergence.md)。
- Causal LM 序列改为自带执行行为和资源生命周期的中性协议，Backend 加载时
  可显式校验 Model 请求的执行协议。
- ONNX 批策略改为综合全部输入/输出 metadata；BGE Model 共用 BERT 构造期与
  输出张量基础契约，统一并发语义并拒绝非有限 Embedding 输出。
- 删除未被生产路径使用的旧 `FixedBatchExecutor` 重载，保留唯一
  `BatchPolicy + BatchSlice` 作者入口。
- 设计依据：[RFC-0021](rfcs/0021-layer4-authoring-and-protocol-convergence.md)。
- 移除不再支持的 Claude Code 与 Gemini CLI 专用 Agent 入口，并将版本演进摘要归档至 `doc/CHANGELOG.md`。
- 将 Node Definition 默认值归一化结果设为 Pipeline 运行时配置的单一事实源。
- 收敛 Operator 绑定/发布事务、Model/Backend Registry 与 BERT 模型公共机制，公共 ABI 和 Catalog 保持不变。
- 统一分片与逐测试模式的必备契约清单并补齐模式间遗漏。
- 设计依据：[RFC-0019](rfcs/0019-high-priority-layer-convergence.md)。
- 收敛请求黑板为稳定只读快照与单次发布契约，保留现有业务的兼容迁移入口。
- 同一 C ABI handle 的 Process/Control 改为串行执行，并明确 Destroy 前停流与等待契约。
- 设计依据：[RFC-0018](rfcs/0018-request-context-and-handle-concurrency-contracts.md)。
- 收敛 Agent、技能、RFC、测试与 GitHub 交付治理，消除重复门禁和架构漂移。
- GitHub 交付默认停在已验证 PR；合并需显式授权，不再回退直接推送 `main`。
- 设计依据：[RFC-0017](rfcs/0017-development-governance-convergence.md)。

## 5.1.0 - 2026-08

- 将完整构建与全部 CTest 收敛为单一质量门禁。
- 由环境管理 ccache，并固定第三方依赖版本与 SHA256 校验值。
- 补齐干净构建依赖和默认测试超时。
- 设计依据：[RFC-0016](rfcs/0016-build-and-test-workflow-convergence.md)。

## 5.0.0 - 2026-08

- 建立 Model / Backend 双注册体系和中性执行协议。
- Embedding、Rerank、LLM、OCR、ASR 改用强类型模型能力契约。
- 移除旧 Engine 路径，并隔离生产配置与确定性测试 Fixture。
- 设计依据：[RFC-0015](rfcs/0015-model-capability-backend-decoupling.md)。

## 4.3.0 - 2026-08

- 新增纯 C11 六级公共日志 API、线程安全进程级阈值和 Demo 环境变量配置。
- 设计依据：[RFC-0014](rfcs/0014-public-log-api.md)。

## 4.2.0 - 2026-08

- 收敛开发与 Sanitizer 构建模式、测试分片及标签化质量门禁。
- 标准上传脚本增加仅创建 PR 的安全交付模式。
- 设计依据：[RFC-0013](rfcs/0013-developer-feedback-loop-acceleration.md)。

## 4.1.0 - 2026-08

- 以 I/O 契约重构通用 Node，并引入显式逻辑端口绑定。
- Validator 增加端口执行契约和同波前写冲突检查。
- 设计依据：[RFC-0012](rfcs/0012-node-authoring-experience.md)。

## 4.0.0 - 2026-08

- 解耦算法 Operator 与底层计算 Platform 的概念、命名和目录结构。
- 设计依据：[RFC-0011](rfcs/0011-operator-platform-naming-unification.md)。

## 3.1.0 - 2026-08

- 将业务相关目录、类型、配置字段和 CLI 术语统一为 `biz`。
- 设计依据：[RFC-0010](rfcs/0010-business-to-biz-naming-unification.md)。

## 3.0.0 - 2026-08

- 引入纯 C11 平台值类型、命名槽位绑定和会话级输出内存池。
- Operator 配置支持模型路径与 Pipeline 配置路径解耦。
- 设计依据：[RFC-0009](rfcs/0009-company-string-and-slot-map-struct-binding.md)。

## 2.1.0 - 2026-08

- Pipeline 直接消费 `ValidatedPipelinePlan`，避免重复解析和 DAG 排序。
- 全部生产节点迁移到浅基类体系，并收敛架构图与验证门禁。
- 设计依据：[RFC-0008](rfcs/0008-architecture-contract-consolidation.md)。

## 2.0.0 - 2026-08

- 建立自描述注册、`BlackboardKey<T>` 强类型黑板和集中式 Pipeline 校验。
- 设计依据：[RFC-0008](rfcs/0008-architecture-contract-consolidation.md)。

## 1.6.0 - 2026-08

- 所有官方 Pipeline 配置迁移为显式 DAG，并移除隐式顺序兼容分支。
- 设计依据：[RFC-0007](rfcs/0007-explicit-dag-standardization-and-legacy-deprecation.md)。

## 1.5.1 - 2026-08

- 修正 Sanitizer、格式化和 C ABI 布局质量门禁，并校准审查证据。

## 1.5.0 - 2026-08

- 交付共享 Catalog/Validator 的 CLI 与可编辑 Web Pipeline Studio。
- 加入安全保存、草稿运行和工作台回归矩阵。
- 设计依据：[RFC-0006](rfcs/0006-visual-pipeline-studio.md)。

## 1.4.0 - 2026-08

- 交付参数化多业务 Demo Runner、Profile 清单和结构化结果输出。
- 设计依据：[RFC-0005](rfcs/0005-parameterized-business-demo-runner.md)。

## 1.3.0 - 2026-08

- 新增 Operator 函数表兼容门面、共享算法运行时和命名 I/O 派发。
- 设计依据：[RFC-0004](rfcs/0004-platform-operator-interface-compatibility.md)。

## 1.2.0 - 2026-08

- Pipeline 改为严格解析和一次性构建状态机，并强化注册冲突防护及结构化诊断。
- 设计依据：[RFC-0003](rfcs/0003-pipeline-dynamic-blackboard-rebaseline.md)。

## 1.1.0 - 2026-08

- 加固 Adapter 的容量、路径绑定、复杂结构和异常安全契约。
- 设计依据：[RFC-0002](rfcs/0002-c-abi-adapter-security-hardening.md)。

## 1.0.0 - 2026-08

- 建立纯 C ABI、四层架构、动态黑板、DAG 调度与定长批处理基线。
- 设计依据：[RFC-0001](rfcs/0001-four-tier-architecture-foundation.md)。
