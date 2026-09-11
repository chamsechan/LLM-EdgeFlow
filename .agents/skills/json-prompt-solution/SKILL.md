---
name: json-prompt-solution
description: Add LLM-EdgeFlow prompt solutions and demos with complete JSON requests and responses at the C ABI boundary. Reuse nodes and text carriers; keep external field selection and response assembly in registered Adapters.
---

# JSON Prompt Solution

用于“完整 JSON 字符串输入 → 指定字段的原文 → 提示词处理 → 完整 JSON 字符串输出”
的 LLM-EdgeFlow 需求。先定位用户的项目 checkout；以下路径相对于仓库根目录。

## 先确定真正的 SDK 边界

本项目业务需求中的输入、输出指完整 C ABI 请求和响应，见根 `AGENTS.md` 与
[业务接入边界](../../../doc/dev_guide/business_onboarding.md#输入输出以-c-abi-为边界)。
传给 `Alg_Process` 输入结构体的字符串必须包含整个请求对象；输出应已由 Adapter
按业务契约组装完成。不能重新把边界解释为 Python、Demo 或内部节点端口。
输入是序列化对象文本，不给整个对象额外添加一层 JSON 字符串编码。

用户明确忽略的 version/endpoint/语言标识等字段不参与语义校验、路由或提示词构造。
业务契约由创建句柄时的注册业务类型与 Pipeline 绑定选定，不能擅自按 endpoint 分发。

## 选择最小实现范围

1. 按 `pipeline-composer` 查询目标构建的 Catalog 和 `describe-node`，核对实际端口、
   模型和现有契约。不要从 prose 推断资源或为新提示词添加专用 LLM 节点。
2. 外部 JSON 字段契约不变时，增加或克隆 Pipeline、必要 `.conf` 和样例即可。
   改变输入/输出字段契约时，按 `llm-edgeflow-developer-guide` 的 Integration 路径，
   在注册 Adapter 内提取输入字段并序列化输出。Demo 只转换载体，不承担该业务语义。
3. 优先复用已有 C 结构、Operator ValueType、结果打包和运行器；有合适载体时无需再建
   平台类型。不同业务可以复用载体，同时注册自己的契约，保持旧业务语义。
   新生产 Adapter 需匹配 bridge，遵循业务接入指南中的注册完整性要求。
4. 按 `CONTRIBUTING.md` 判断 RFC；追加 C ABI 业务类型需记录接口决定，普通配置不需
   额外审批。算法能力缺失时才考虑 custom Node，不把平台转换放进 Core 或 Nodes。

当前翻译参照 `doc/solutions/translate.md`、`src/adapter/biz/translate_adapter.cpp` 和
`configs/pipeline_translate_cpu.json`。其 `translate_v1` 复用既有文本/JSON 载体、一个
`LlmGenerateNode` 及模型实现。`sentence_text` / `entities_json` 是载体字段名称，不是业务
JSON 内的 query / translated；名称不够通用并不要求全仓改名。

## 编排与字符串保真

- 克隆兼容方案，声明节点 `id`、`depends_on` 和真实绑定。把提示词与生成参数放配置里。
- 原文包含问句或指令时仍是处理对象。不要把完整请求交给模型自行选择字段；Adapter
  仅把约定原文交给提示词。翻译样例由 C++ JSON 解析器读取原始 `std::string`，经
  `TextBatch` 直接交给生成节点；模型配置中的 system prompt 说明处理规则。
- 输出语义只有一个字符串时，优先让模型只生成结果原文，每条请求只调用一次生成。
  C++ Adapter 用 JSON 序列化器组装字段，正确处理引号、换行、反斜杠和 NUL；不要用
  模型读取字段、拼协议、修复格式，也不要对模型原文擅自 trim、解析或去除包装。
  该单次生成指一次文本生成调用，不是自回归解码只运行一个 token。
- 只有业务本身需要模型产生结构化语义时才引入结构化解析节点，校验字段与类型并使用
  `failure_policy=fail`。SDK 响应组装始终留在 Adapter，不靠 Demo 投影字段。
  不用固定示例或 fallback 冒充成功。
- `.conf` 必须指向新 Pipeline，检查模型路径覆盖、上下文与生成长度，以及 C ABI 固定
  输出数组和 Operator 池容量的区别。原 Profile 不会自动指向新方案。

## 验证与交付

- 重建注册，使用生产 `alg_pipeline_tool` 执行 Catalog、validate、plan、resolve-conf。
  有意使用测试资产时全程选测试工具，不用测试工具绕过生产失败。
- 业务 I/O 契约变化时，扩展现有 Adapter/C ABI 套件，直接调用 `Alg_Process` 输入完整对象。
  可用计数测试模型确定性验证原文输入、单次生成和 C++ 响应组装，再运行真实模型确认效果。
  Operator Demo 共用 Adapter，但不等于调用过导出的 C ABI 入口；报告时区分两者。
- 覆盖无关字段变化、缺失/非字符串字段、转义和空串、输出类型、来源、容量与失败状态。
  不为每个提示词创建新测试框架或执行文件；沿用已有套件。
- 用本次配置运行用户样例，检查原生 ID、逐条状态、汇总和最终 JSON；再执行最终门禁。
  没有权重时明确真实效果未验证，按项目资产清单准备资源，不把替身输出当效果。
- 按根 `AGENTS.md` 分别委派测试编写、编译、测试执行给不同子 agent，遵循源码依赖顺序，
  由主 agent 实现和复核；交付流程仍以 `CONTRIBUTING.md` 为准。
- 交付一条可复制命令、C ABI 实际输入/输出示例、配置位置和准确的修改范围。
  若复用旧载体名，说明其含义。未经用户要求不上传或创建 PR。

本 skill 保存复用决策，不维护节点/模型 Catalog；后续相似业务继续更新这一份指引。
