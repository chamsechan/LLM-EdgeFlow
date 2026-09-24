# RFC 0075：模型路径单一来源

- **RFC 编号**：0075-model-path-single-source
- **创建日期**：2026-09-24
- **文档状态**：Completed
- **关联分支**：`refactor/model-config-dedup`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：取代 RFC-0061 的 `deployment.model_paths` 覆盖机制及 RFC-0062 对该覆盖来源的记录和诊断投影；遵循 RFC-0074 的当前配置严格校验。

## 1. 问题与范围

同一 Pipeline 的模型路径同时存在于 `models[].model_path` 和
`deployment.model_paths[model_id]`，后者覆盖前者。作者修改模型条目后可能仍运行旧路径，
工具还必须同步模型重命名、路径来源和诊断。本次将模型路径集中到模型条目，删除覆盖机制。
涉及接入适配层 / Integration、原生工具、Studio、示例、测试及当前文档。
流程编排层 / Orchestration 继续消费中性模型配置；模型算法与公共 Operator ABI 不变。

## 2. 决策与不变量

- `models[].model_path` 为唯一模型路径字段；`deployment` 仅允许 `io`。
- 不再存储覆盖 ID 集合或路径来源映射。模型路径诊断和修复直接定位 `/models/<index>/model_path`。
  派生业务身份的诊断仍映射至 `/deployment/io/io_binding`，不可产生编辑根业务名的修复。
- 相对路径仍以宿主显式模型根目录为基准，保留目录穿越、绝对路径越界和符号链接逃逸检查。
  词法校验不检查资源存在，实际部署解析保留现有根目录要求。
- `resolve-conf` 的观测报告仍可列出 `configuration.model_paths`，其来源统一为
  `pipeline.models.model_path`；报告字段不是可持久化的第二份配置。
- Studio 保存、模型编辑/重命名、Recipe 和效果验证只读写模型条目的路径。
  资产清单路径仍相对资产目录，工具显式区分资产目录与宿主根，选择资产时写入宿主根相对路径。
  Sidecar 路径按实际模型目录解析，不猜测路径、不搜索回退。
- `model_config` 与 `backend_config` 分属模型语义和后端执行参数，维持独立职责；
  本次不引入共享模板、继承、默认值系统或额外配置字段。

## 3. 兼容与迁移

删除的 `deployment.model_paths` 按普通未知字段拒绝，不提供双读、回退或静默迁移。
对仓内有效配置，将旧映射的实际生效值复制到相同 `model_id` 的 `model_path`，再删除映射；
未覆盖的模型保持原值。Mock 实体抽取的两个夹具移除陈旧真模型覆盖，保留已由测试资产清单
固定的 `neutral-llm.fixture`，使配置和效果验收使用同一中性资源。
配置根目录及 `.conf.pipe_path` 不变。测试中的非法旧字段可保留。
更新所有配置消费者及现行指南，历史 RFC 和历史变更记录保留。回退需将实现与配置整体回退。

## 4. 验证与完成条件

- 原生解析、导出 Schema、CLI 与 Operator 拒绝旧映射；直接模型路径的空值、类型错误、
  多模型、路径边界和诊断均有覆盖。
- 验证生产配置迁移前后的实际路径一致；Studio 保存/重命名/删除/运行和 Recipe 不重建覆盖表。
- 模型路径的可应用修复保留，派生业务字段修复继续过滤；失败不发布部分部署结果。
- 使用当前构建验证受影响配置及 plan，并执行 Mock 模型路径，检查请求 ID、状态和输出。
  可选后端未启用、真实模型质量和目标硬件验收分别报告。
- 独立审查与 CONTRIBUTING 规定的唯一最终门禁通过后完成。

## 5. 实施与最终结果

- 删除配置中的路径覆盖表、解析状态与模型路径诊断投影；Schema、CLI、Studio、Recipe 与效果检查同步使用模型条目。
- 26 份配置删除重复字段；16 份生产配置的 31 条生效模型路径完全保留。
  Mock 实体抽取使用固定中性 fixture，其他路径按原生效值迁移。
- 当前默认构建的 10 份生产配置与 9 份 Mock 配置均通过 validate / plan；另 7 份生产配置依赖
  未启用的 Kite / Whisper Backend，返回 UNKNOWN_BACKEND，未冒充该构建可用。
- 实体抽取请求 30001 与文档问答请求 10001、10002 实际运行 status 为 0，输出字段完整。
  resolve-conf 的路径与原生产资源位置一致，来源统一为 pipeline.models.model_path。
- 7 组原生聚焦 CTest 通过。Studio Python 套件 102 项完成（默认跳过浏览器项），
  Recipe 23 项通过；另显式配置 Playwright / Chromium 执行浏览器流程通过。
  新测试覆盖嵌套资产模板经前端转换后的主文件与 sidecar、相对 sidecar 越界，以及外部目录
  Recipe 的模型重基与实际效果运行。
- 独立审查发现的嵌套资产 sidecar 重复目录问题已修复并复审关闭；当前无剩余阻断。
- 最终门禁采用 `LLM_EDGEFLOW_JOBS=6 ./scripts/run_all_tests.sh`；Completed 状态与最终改动一起
  交门禁验证，失败恢复实施状态。真实模型效果与目标硬件验收不在本次范围。
- 现行配置说明见 [configs/README.md](../../configs/README.md)，资源根目录说明见
  [效果验证指南](../VERIFIABLE_SELECTION.md)。
