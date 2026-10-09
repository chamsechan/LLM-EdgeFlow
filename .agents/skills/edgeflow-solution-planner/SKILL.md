---
name: edgeflow-solution-planner
description: 根据业务输入输出，为 LLM-EdgeFlow 选择现有能力、识别需新增的 Adapter/Node/Model/Backend，给出带类型端口的 DAG、配置建议和开发顺序。适用于需求拆解与方案设计；已有明确配置修改用 pipeline-composer。
---

# 从业务需求到组件和 DAG

面向“给定一个业务，项目里要增加什么、如何连线”的任务。以当前代码、目标构建的
Catalog 和完整 Operator 请求/响应为依据；流程与设计边界遵循
[CONTRIBUTING](../../../CONTRIBUTING.md)。只请求方案时交付建议；同时请求实现时，
完成方案后继续对应开发和验证，不把路由到另一个 skill 当作完成。

## 确定业务边界

从用户提供的样例提取完整请求、完整响应、字段语义、每请求输出数量、失败行为及
模型/部署约束。区分已知要求、可用默认值和待确认项。只有会改变接口或方案选择的
缺失信息才需要询问，其余先给出明确假设。不要让 Demo 提取业务字段或拼装协议。

读取[业务接入边界](../../../doc/dev_guide/business_onboarding.md#输入输出以-operator-接口为边界)
及与业务相关的现有 Converter/方案；同一载体结构不证明 JSON 字段或语义兼容。

## 查能力并选择最小增量

使用同一个目标构建的工具；下面 `build` 仅代表已配置构建目录。先查询，再只描述候选能力：

```bash
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool catalog
./build/alg_pipeline_tool describe-node <node_type>
./build/alg_pipeline_tool describe-model <model_type>
./build/alg_pipeline_tool describe-backend <backend_type>
```

只有选定已存在的 binding 才使用过滤查询。查不到能力时先区分未注册、工具陈旧、构建
未启用 Backend 和确实缺少实现。需要时重建目标工具；不能用测试注册掩盖生产缺口。

| 需求差异 | 最小修改与后续 skill |
| --- | --- |
| 端口、业务契约与能力均匹配，只调整提示词/连线/模型实例 | [pipeline-composer](../pipeline-composer/SKILL.md) |
| 外部字段提取、序列化、容量、载体或业务契约不同 | [edgeflow-adapter-developer](../edgeflow-adapter-developer/SKILL.md) |
| 新增逐项变换、文本 LLM 前后处理、多输入/输出、拆分/聚合/排名或其他模型调用算法 | [edgeflow-node-developer](../edgeflow-node-developer/SKILL.md) |
| 现有运行协议可用，缺少模型预处理或输出语义 | [edgeflow-model-developer](../edgeflow-model-developer/SKILL.md) |
| 缺少厂商运行时/硬件执行支持 | [edgeflow-backend-developer](../edgeflow-backend-developer/SKILL.md) |
| 现有调度、类型或生命周期机制不能表达必要契约 | [developer guide](../llm-edgeflow-developer-guide/SKILL.md) 的 Core 路径，先举证缺口 |

每个新增项写明“现有哪部分不满足”及实现位置。新业务不自动需要新 Node；新权重不自动
需要新 Model；新 Model 不自动需要新 Backend。common/custom 是算法归属，按中性操作
或领域算法选择；不把业务名固化进 Node，也不为复用而强行泛化。

## 给出可以落到配置的 DAG

- 图的边界画出 Adapter ingress/egress，但只有真实能力节点进入 `pipeline` 数组。
  Model/Backend 是节点引用的执行资源，单列绑定关系，不当成 Pipeline 节点。
- 同时给 Mermaid 图和连线表：节点 `name` / `type`、逻辑端口、来源引用、批类型、
  数量关系与 `(req_id, sub_id)` 保留/生成策略。标注哪些类型已注册、哪些尚待实现。
- 节点 `inputs` 显式引用 `节点名.端口名` 或 `input.端口名`；输出项 `inputs` 连接回包来源。数据依赖由 Validator 推导。
  `depends_on` 仅表达额外顺序，数组顺序不是依赖。检查分支汇合的来源关系和最终 egress 闭合。
- `params` 的模型引用字段填写 `models[].name`；能力来自 Model Definition。
  并发先沿用默认 `max_parallel_workers=1`；需要并行时检查 Node、Model、Backend 的真实
  声明，再用 plan 验证，不把拓扑分层直接等同于可并行执行。
- 部署在根 `io.input` / `io.output` 选择 `{type, name, params?}`；`.conf` 只用 `pipe_path` 定位 JSON。
  模型路径属于 `models[].file`。复用现有合法配置，避免猜测容量字段或模型参数。
  模型条目为 `{type, name, file, params?, backend: {type, params?}}`；类别与后端选出实现。
  文件参数相对 Pipeline JSON 目录，生成选项和转写语言放节点，向量归一化放模型。

全为现有能力时，输出候选 Pipeline 并用目标工具 `validate`、`plan` 核实图与计划。
缺少注册时，给出明确标为“待实现”的结构草案和未通过原因；不要声称草案可以运行。

## 交付内容

规模小的需求也保留以下信息，但无需独立长篇设计文档：

1. 完整输入/输出样例与业务假设。
2. 组件清单：复用/修改/新增、原因、具体目录/注册入口、对应 skill。
3. DAG、类型化连线及模型绑定，必要的 JSON 配置或待实现草案。
4. 最短开发顺序与验收：先实现缺失注册，再重建 Catalog、完成配置、validate/plan、
   Operator 边界测试及用户样例运行。配置验证、Mock 路径、真实模型效果分别报告。

执行配置部分继续 [pipeline-composer](../pipeline-composer/SKILL.md)；完整 JSON 提示词业务
按需读取 [json-prompt-solution](../json-prompt-solution/SKILL.md) 的字符串与协议规则。
