# RFC 0048: 完整 JSON 翻译请求进入 SDK

- **RFC 编号**：0048-translation-json-abi
- **创建日期**：2026-09-11
- **文档状态**：Completed
- **关联分支**：`feat/translate-prompt-solution`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow contributors

## 1. 问题与范围

翻译输入是完整 JSON 对象字符串。Demo 在 SDK 外提取 `query` 会让直接使用 C ABI 的
调用方得不到同一功能，因此提取与响应组装必须属于接入适配层 / Integration。
每条请求只调用一次模型生成译文，输入字段读取和输出 JSON 构造均由 C++ 完成。

## 2. 决策与权衡

新增 `ALG_BIZ_TYPE_TRANSLATE = 8` 和 `translate_v1` Adapter。沿用
`CompanyEntityInputStruct` / `CompanyEntityOutputStruct` 文本载体及现有 typed keys，
其 `sentence_text` 在翻译契约中承载完整 JSON，`entities_json` 承载完整响应 JSON。
复用 Entity Adapter 的边界校验、copy-in、来源检查和两种结果打包；翻译 Adapter 只增加
JSON 对象校验、`query` 字符串选择和仅含 `translated` 字符串的响应序列化。
输入语法与 query 类型之外的字段不参与路由或语义校验。

新增 bridge 复用 `entity_in/entity_out` 宿主类型与输出池，无新增 ValueType；统一 Demo
复用已有文本/JSON 运行函数，只注册 translate 名称。Pipeline 仅使用现有
`LlmGenerateNode`，Model 的 system prompt 配置要求只输出译文原句。Core、节点、Model、
Backend 实现以及六个 C ABI 函数均不变。增加业务枚举使宿主在 Create 时明确选择契约，
避免按请求 endpoint 分发，或改变原有实体业务的纯文本语义。

调用方在同步调用期间持有完整输入字符串；Adapter 不跨请求保存值。输入对象上限沿用
现有载体的 64 KiB；解析后的原始 query 通过 `TextBatch` 进入唯一一次文本生成。
模型结果通过 `kLlmAnswers` 原样进入 Adapter，C++ 用 JSON 序列化器产生响应，不解析模型
JSON，也不裁剪文本或调用模型修复格式。单次生成不限制模型内部的自回归 token 解码。
C ABI 输出仍为 2048 字节数组（含结尾 NUL），超出报错；Operator 使用配置中的变长池容量。

## 3. 兼容与迁移

追加枚举，不修改已发布结构布局、函数签名或既有契约。翻译 Pipeline 改用
`translate_v1`，宿主 Create 使用新业务类型；Demo 将完整请求送入 SDK，不再提取字段。
仅适用于当前本地平台模拟声明，真实公司 SDK 接入继续遵循 RFC-0029。

## 4. 验证与完成条件

在已有 Adapter/C ABI 套件验证完整对象输入、忽略字段、转义、非法 JSON/query、来源与
容量错误；直接调用 `Alg_Process` 配合计数测试模型验证原始 query、每条请求一次生成、
C++ 响应组装与译文原样保留，避免仅验证 Python。
Operator 完整注册检查、翻译真实模型 Demo、Catalog validate/plan 和完整门禁通过。

## 5. 实施与最终结果

已完成 Adapter、bridge、单节点 Pipeline、Demo 注册与完整请求转发；现有 C ABI 套件
两项翻译测试验证原始 query、单次生成、原文保真、来源、容量与失败不重试。
真实模型直接 C ABI 调用返回 `{"translated":"你好，你叫什么名字？"}`，Operator Demo
三条请求成功；空原文由当前 Qwen Model 返回错误。Catalog、validate/plan、部署解析
与 skill 校验通过。测试编写、编译和执行已分别交给三个子 agent，规则写入 `AGENTS.md`。
最终完成以本次完整门禁通过为准；真实公司 SDK 与目标硬件仍不在本次范围。
[翻译方案](../solutions/translate.md)与复用 skill 已同步 SDK 边界与 C++ 确定性协议处理。
