# Changelog

## Unreleased

LLM-EdgeFlow 尚未正式发布。当前产品版本为 **11.0.0**，表示当前构建与接口基线。
研发过程和迁移历史通过 Git 追溯；正式发布后按版本记录用户可感知的能力与契约变化。

当前能力：

- 错误码收敛为中立框架码和平台对外码两套；内部取消裸码与旧异常码兼容参数，
  Studio/CLI 使用同一错误名称表。Operator 在失败出口记录分级日志与内外码，
  中间层保留具体诊断。见[错误码与日志](dev_guide/error_handling.md)。

- 四层架构采用单向依赖，C++ Operator 是宿主调用入口。协议壳和平台 binding 负责载体与内存布局，
  Converter 负责完整请求和响应；内部类型不承担外部协议。见[架构与接入边界](architecture.md)。
- Pipeline 显式连接 typed 端口；Validator 生成执行计划，运行时消费计划。模型类别与 Backend
  协议选择唯一实现，文件相对 Pipeline 文档目录解析。见[配置说明](../configs/README.md)。
- 通用和自定义 Node 使用统一的函数与 Spec。失败保留原始原因、错误码及批次来源；
  参数更新使用原子快照，失败保持原值。会话模型注册保留身份校验和批量安装事务，
  缓存按会话、模型名和输入隔离。见[Node 作者指南](dev_guide/custom_node_concepts.md)。
- 当前业务响应使用统一 JSON 输出载体；共享容量声明，保留按行关联、池预算和失败不发布输出。
  新类型与命名布局通过登记扩展。见[输出池与生命周期](dev_guide/operator_output_allocation.md)。
- 模型执行层提供通用 BPE 分词器 `text::BpeTokenizer`：读取开发者用 `tools/prepare_bpe_tokenizer.py`
  从 Hugging Face `tokenizer.json` 离线生成的 `.eftok` 文件，规范化、预分词、字节回退、合并优先级和解码器由文件头参数选择，
  与具体模型无关；工具对不支持的组件明确报错，`score` 优先级只在能证明与 HF 合并顺序等价时生成。
  见[BPE 分词器文件格式](dev_guide/bpe_tokenizer_format.md)。
- 原生 CLI 提供 Catalog、配置校验、计划与部署解析；Studio 使用同一构建的能力声明。
  HTTP 请求字段使用 `revision`、`filename`、`conf_name`。Demo 套件使用 `alg_demo --suite`。
  见[Studio](../tools/pipeline_studio/README.md)和[统一 Demo](dev_guide/business_onboarding.md#5-统一-demo-接入)。

验证范围：默认开发门禁覆盖编译、契约与行为测试、SDK/Demo smoke、CLI/Studio 工具和架构守卫。
真实模型执行、浏览器交互、公司内部 SDK 及目标设备需要各自的资产、环境和验收证据；
默认门禁通过不替代这些验收。详见[验证范围与发布准备](VERIFIABLE_SELECTION.md#验收范围与发布准备)。
