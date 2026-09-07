# RFC-0039：自定义节点开发路径与失败契约修复

- **RFC 编号**：0039-custom-node-authoring-closure
- **创建日期**：2026-09-06
- **文档状态**：Completed
- **关联分支**：`fix/custom-node-authoring-closure`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow 维护者

## 1. 背景与目标

方案开发者需要完成连线、局部领域算法和平台数据转换。现有 custom_nodes 目录、浅层
Node 支持类和 Demo 自注册符合这一方向，但脚手架输出使用不存在的接口，样例把推理
失败转成成功，且两方案复用仅做了局部测试，无法作为可用性验收。

本次补齐“生成源码 → 编译注册 → 原生校验 → 执行 → Adapter 输出”的路径；继续按操作
平铺自定义节点，不引入插件系统、业务目录树或新的运行时抽象。

## 2. 范围与架构边界

- Layer 1 / Demo：沿用既有 Adapter 和外部结构；Demo 类型完全来自注册描述符。
- Layer 2：沿用 Validator、计划和类型化 Blackboard；为已有 ScoreBatch 补充类型标识。
- Layer 3：修复 PromptGuidedLlmNode，保留无请求状态、显式模型绑定和溯源检查。
- Layer 4：复用既有能力接口和测试模型，不修改模型或 Backend 实现。
- Tooling：生成器使用真实 API；生成源码和测试进入现有测试 runner 的编译与执行。

实际 SDK / 硬件验收遵循 RFC-0029。测试模型只能证明集成路径，不能证明业务准确率。

## 3. 关键决策与兼容性

1. 删除样例的 `fallback_text`，配置显式拒绝旧字段。推理失败、数量不符或溯源不符均
   返回失败且不发布输出。需要业务降级时，应另行设计携带明确状态的输出契约。
2. 模板只解析原始模板；输入、上下文和 system_prompt 中的内容不会再次替换。
   支持 `{input}`、`{context}`，字面花括号写作 `{{` / `}}`；非法占位符拒绝初始化。
   使用 `{context}` 必须连接 context，按 req_id 汇集文档；删除默认模板隐式追加上下文。
3. Definition 与初始化共用参数解析，严格检查 stop_words 和生成参数。空输入不调用模型。
4. 脚手架生成 compute、model、unary_inference 骨架。模型骨架的批类型必须匹配能力；
   当前 unary 支持类不能保持 ImageRefBatch 的独立容器类型，因此 OCR 使用 model 骨架，
   对 unary OCR 明确报错。异类型 compute 或非 1:1 逻辑留下显式失败的待实现入口。
   默认 parallel_safe=false，完成并发审查后再由作者启用。
5. Demo 注册必须给出非 UNKNOWN 类型；无注册即未知，不保留中央业务名兜底分支。

以上修复针对尚在开发分支的能力。旧配置删除 fallback_text；默认追加 context 的用法
改为显式 `{input}\n{context}`；模板内字面 JSON 花括号需要转义。

## 4. 验证与验收

- 脚手架：CLI 输入、写入保护；生成所有受支持模板，真实编译、注册并执行；验证错误
  传播、类型和溯源，并用原生 Validator 校验生成节点连线。
- 样例：输入含占位符、跨请求上下文、空输入、坏参数、模型失败、数量和溯源故障注入。
- 复用：实体抽取和文档问答使用同一节点，两份严格注册业务 Pipeline、conf 和 smoke
  profile，通过统一 Demo 执行并断言输出内容及 request_id。
- Demo：新增别名无需中央分支、缺失类型拒绝，测试后恢复全局注册状态。
- 交付：`./scripts/run_all_tests.sh`。完成后更新本 RFC 和方案开发者规划中的验收状态。

### 验证结果（2026-09-06）

- `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh` 通过：90/90 个 CTest 分组成功，
  统一门禁耗时 72 秒，包含格式、分层与文档检查、默认 Backend 构建及业务回归。
- 生成 11 份 C++ 骨架与注册测试（compute 两种、model 五种、unary 四种），进入
  既有 Node runner 编译；所有受支持能力通过实际类型绑定、批处理和来源检查。
- CommonNodesTest / CustomNodeScaffoldTest / DemoRunnerTest 专项通过；模型报错、
  数量错误、req_id / sub_id 错位和缺失输入均不发布成功输出。
- 两份 custom Pipeline 的原生 validate / plan 无诊断；统一 `alg_demo` 执行实体样本
  `30001` 和问答样本 `10001`、`10002` 成功，集成测试断言具体内容、意图与来源。
- 生产 Catalog 能发现 PromptGuidedLlmNode；生成的测试节点只链接测试 runner。

## 5. 风险与后续

提示词样例用于展示完整开发路径；已有通用节点能满足需求时仍优先连线。具体领域算法
需真实业务输入、期望输出及验收样本驱动，不把示例包装成新的生产能力。新增外部结构
继续使用 Adapter / Operator bridge 扩展路径，本次不虚构新的平台协议。
