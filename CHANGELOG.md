# Changelog

本文档记录 LLM-EdgeFlow 的架构里程碑与用户可感知变更。完整的设计动机、接口细节和验收证据由对应 RFC 维护；测试数量等易过时信息不在此重复记录。

仓库当前尚未发布对应 Git tag，因此以下版本号表示项目里程碑，而非可下载的正式 Release。

## Unreleased

- 暂无已确认变更。

## 5.1.0 - 2026-08

- 将完整构建与全部 CTest 收敛为单一质量门禁。
- 由环境管理 ccache，并固定第三方依赖版本与 SHA256 校验值。
- 补齐干净构建依赖和默认测试超时。
- 设计依据：[RFC-0016](doc/rfcs/0016-build-and-test-workflow-convergence.md)。

## 5.0.0 - 2026-08

- 建立 Model / Backend 双注册体系和中性执行协议。
- Embedding、Rerank、LLM、OCR、ASR 改用强类型模型能力契约。
- 移除旧 Engine 路径，并隔离生产配置与确定性测试 Fixture。
- 设计依据：[RFC-0015](doc/rfcs/0015-model-capability-backend-decoupling.md)。

## 4.3.0 - 2026-08

- 新增纯 C11 六级公共日志 API、线程安全进程级阈值和 Demo 环境变量配置。
- 设计依据：[RFC-0014](doc/rfcs/0014-public-log-api.md)。

## 4.2.0 - 2026-08

- 收敛开发与 Sanitizer 构建模式、测试分片及标签化质量门禁。
- 标准上传脚本增加仅创建 PR 的安全交付模式。
- 设计依据：[RFC-0013](doc/rfcs/0013-developer-feedback-loop-acceleration.md)。

## 4.1.0 - 2026-08

- 以 I/O 契约重构通用 Node，并引入显式逻辑端口绑定。
- Validator 增加端口执行契约和同波前写冲突检查。
- 设计依据：[RFC-0012](doc/rfcs/0012-node-authoring-experience.md)。

## 4.0.0 - 2026-08

- 解耦算法 Operator 与底层计算 Platform 的概念、命名和目录结构。
- 设计依据：[RFC-0011](doc/rfcs/0011-operator-platform-naming-unification.md)。

## 3.1.0 - 2026-08

- 将业务相关目录、类型、配置字段和 CLI 术语统一为 `biz`。
- 设计依据：[RFC-0010](doc/rfcs/0010-business-to-biz-naming-unification.md)。

## 3.0.0 - 2026-08

- 引入纯 C11 平台值类型、命名槽位绑定和会话级输出内存池。
- Operator 配置支持模型路径与 Pipeline 配置路径解耦。
- 设计依据：[RFC-0009](doc/rfcs/0009-company-string-and-slot-map-struct-binding.md)。

## 2.1.0 - 2026-08

- Pipeline 直接消费 `ValidatedPipelinePlan`，避免重复解析和 DAG 排序。
- 全部生产节点迁移到浅基类体系，并收敛架构图与验证门禁。
- 设计依据：[RFC-0008](doc/rfcs/0008-architecture-contract-consolidation.md)。

## 2.0.0 - 2026-08

- 建立自描述注册、`BlackboardKey<T>` 强类型黑板和集中式 Pipeline 校验。
- 设计依据：[RFC-0008](doc/rfcs/0008-architecture-contract-consolidation.md)。

## 1.6.0 - 2026-08

- 所有官方 Pipeline 配置迁移为显式 DAG，并移除隐式顺序兼容分支。
- 设计依据：[RFC-0007](doc/rfcs/0007-explicit-dag-standardization-and-legacy-deprecation.md)。

## 1.5.1 - 2026-08

- 修正 Sanitizer、格式化和 C ABI 布局质量门禁，并校准审查证据。

## 1.5.0 - 2026-08

- 交付共享 Catalog/Validator 的 CLI 与可编辑 Web Pipeline Studio。
- 加入安全保存、草稿运行和工作台回归矩阵。
- 设计依据：[RFC-0006](doc/rfcs/0006-visual-pipeline-studio.md)。

## 1.4.0 - 2026-08

- 交付参数化多业务 Demo Runner、Profile 清单和结构化结果输出。
- 设计依据：[RFC-0005](doc/rfcs/0005-parameterized-business-demo-runner.md)。

## 1.3.0 - 2026-08

- 新增 Operator 函数表兼容门面、共享算法运行时和命名 I/O 派发。
- 设计依据：[RFC-0004](doc/rfcs/0004-platform-operator-interface-compatibility.md)。

## 1.2.0 - 2026-08

- Pipeline 改为严格解析和一次性构建状态机，并强化注册冲突防护及结构化诊断。
- 设计依据：[RFC-0003](doc/rfcs/0003-pipeline-dynamic-blackboard-rebaseline.md)。

## 1.1.0 - 2026-08

- 加固 Adapter 的容量、路径绑定、复杂结构和异常安全契约。
- 设计依据：[RFC-0002](doc/rfcs/0002-c-abi-adapter-security-hardening.md)。

## 1.0.0 - 2026-08

- 建立纯 C ABI、四层架构、动态黑板、DAG 调度与定长批处理基线。
- 设计依据：[RFC-0001](doc/rfcs/0001-four-tier-architecture-foundation.md)。
