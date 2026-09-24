# RFC 0071：删除 Pipeline 配置的重复概念与隐式绑定

- **RFC 编号**：0071-pipeline-configuration-simplicity
- **创建日期**：2026-09-23
- **文档状态**：Completed
- **关联分支**：`refactor/pipeline-config-simplicity`
- **目标版本**：投产前
- **负责人 / 作者**：LLM-EdgeFlow maintainers
- **关联决策**：取代 RFC-0007 的必填显式数据依赖、RFC-0057 的工具维护重复依赖，以及 RFC-0067 对旧配置结构的保持要求；其他职责与安全契约继续有效。

## 1. 问题与范围

同一数据连接由输入映射、输出映射及 depends_on 重复表达；ports 包装、模型 capability、
execution_mode 和必需输入的同名隐式绑定增加理解负担。用户明确授权在上线前重构，要求
优先删除或合并概念，避免引入新引用语法、after、egress、并发字段或第二种持久化格式。

本次修改 Pipeline 配置、流程编排层的校验规划、工具、配置样例和相应测试文档。
完整 Operator 请求响应、Adapter 转换职责、Node 算法、模型执行协议与资源生命周期不变。
不增加共享模型自动串行调度、复合节点或兼容解释器。

## 2. 决策与权衡

1. Node 保留 id、node_type、config；inputs / outputs 从 ports 提升到节点顶层，映射值
   仍为既有数据 key。outputs 的既有默认逻辑端口 key 保持，输入一律明确连接：缺必需
   输入报错，缺可选输入表示未连接，依赖或同名数据均不得使其自动接入。
2. depends_on 可省略，仅用于额外执行顺序。PipelineValidator 根据已声明输出的有效 key
   和显式输入映射推导唯一生产者依赖，再与显式 depends_on 合并去重后校验、规划。
   JSON 数组顺序不表达依赖。显式重复/未知依赖、数据自环/普通环、联合依赖环仍拒绝。
   缺生产者、多生产者、与 ingress 重复发布、未知端口和类型/数量/来源/生命周期错误仍拒绝。
   无歧义来源必须先确定，不能按数组顺序选择生产者。
3. models[].capability 删除；能力只来自注册 ModelDefinition。Node 模型引用必须显式
   填写；删除作者 Model helper 的约定实例名默认值，注册检查同样拒绝可选或带默认值的
   模型引用字段。普通算法参数的 Definition 默认值保持。
4. execution_mode 删除；仅保留 max_parallel_workers，范围 1–64、默认 1。大于 1
   启用现有并行运行及其安全检查。串行模型冲突继续拒绝，不增加自动资源串行调度。
5. PipelineValidator 是唯一推导与验证所有者，ValidatedPipelinePlan 保存可执行结果，
   Pipeline 不重解析。Studio/CLI 消费原生结果；连接操作只维护数据映射，不再写入数据
   depends_on。移除已无意义的补数据依赖和恢复隐式输入绑定操作；显式顺序依赖操作保留。
   图形展示可以显示原文连线，但不以独立规则声称配置合法或可执行。

## 3. 兼容与迁移

运行时只接受新格式，ports、execution_mode、models[].capability 作为未知字段拒绝。
不新增别名、格式协商、自动迁移入口或持久编译产物。仓内生产/Mock 配置、测试构造器、
Schema、Studio、脚本与现行文档一次迁移；历史 RFC/验收记录保留原文。
现有数据依赖从样例中移除，纯顺序约束保留。旧并行缺省对应显式 workers=4，串行采用 1；
隐式必需输入迁移为明确同名映射，模型绑定补齐既有选择，避免业务结果变化。
回退单位是此分支的完整修改集，不能混用新工具、旧配置或相反组合。

## 4. 验证与完成条件

- Core：无 depends_on 的乱序链/分支汇合与显式顺序依赖正确规划；缺必需输入、可选输入
  未接入、自环/联合环、多生产者、ingress 冲突和旧字段拒绝；模型能力推导、模型引用必填、
  workers 缺省/范围及共享串行模型保护。
- Tooling：Schema、init、edit、validate、plan 和 Studio 使用单一新格式；连接/断开
  不残留数据依赖，可选端口不被自动连接，独立顺序约束不随断线删除。
- 迁移：生产配置按可用 Backend 原生校验，Mock 使用测试构建；执行迁移后的代表性无模型
  与 Mock 业务方案并检查请求 ID、状态和完整结果。缺 Backend/模型资产明确报告。
- 独立审查 Core/跨层变更和测试证据；最终运行 CONTRIBUTING 的唯一交付门禁。

## 5. 实施与最终结果

三个独立工作包可并行开发：Core/Node 契约、工具消费、配置及测试迁移。先通过 Core 聚焦
验收，再验收工具和业务执行，最后完成独立审查及统一门禁。失败回到相应工作包修复；
不将初次实现或仅编译成功计为完成。真实模型质量和目标硬件不属于本次行为保持验收。

已完成三项工作包的实现与迁移：

- 26 份生产/Mock 方案共删除 91 条与数据连接重复的显式依赖；已对迁移前后配置做业务字段
  等价比较，无纯顺序约束丢失。默认输出名继续有效，输入与模型引用全部显式。
- Core 聚焦覆盖乱序链/钻石执行、额外顺序合并、数据/混合/自身环、歧义生产者、必需与
  可选输入、模型绑定和 workers 安全检查。Parser 明确拒绝三类删除的旧字段。
- 工具删除补依赖及恢复隐式绑定操作；连接和断开只编辑数据映射，额外顺序保留。
  新增混合环的草稿/严格提交回归；Schema、生成器、静态查看器、Studio 和现行指南已迁移。
- 默认构建下 19/26 份配置通过原生验证；剩余 7 份使用未启用的 kite_llm/whisper_cpp，
  未计作通过。生产配置始终使用生产 Catalog，Mock 始终使用测试 Catalog。
- 直接执行迁移后的 keyword_match_rules、doc_qa_mock、entity_extract_custom_mock，
  共 5 条请求全部成功，检查了请求 ID、状态和完整结果。输出见
  `/tmp/edgeflow-rfc0071-{keyword,docqa,entity}/`；未执行真实模型质量或目标硬件验收。
- 独立只读审查未发现阻断问题；建议的混合环编辑测试和旧模型 capability 拒绝测试已补齐。
  真实 Chromium Studio 回归已通过，截图位于 `/tmp/rfc0071-studio-screenshots/`。

验收记录：

- 受影响的 19 项 Core/Node/Adapter/Tooling CTest 全部通过；其中接入覆盖测试的旧 capability
  必填断言改为原始 model_path 缺失，即使部署覆盖提供路径也必须拒绝，复测通过。
- Studio 96 项测试全部通过，包含真实浏览器、无跳过；Recipe 21 项与 3 个 JS 模块通过。
- 最终门禁使用 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`，同时传入已安装的
  Playwright/Chromium 路径以执行浏览器回归。日志保存于
  `/tmp/edgeflow-rfc0071-final-gate.log`。依 CONTRIBUTING，最终 diff 先准备 Completed，
  完成状态由该门禁成功确认；失败则恢复 In Implementation 并修复。

现行用法见 [配置说明](../../configs/README.md#配置中的连接与模型) 与
[Studio 指南](../../tools/pipeline_studio/README.md)。本次为本地交付，未执行远程发布。
