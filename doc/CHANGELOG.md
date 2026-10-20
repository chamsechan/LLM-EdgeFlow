# Changelog

## Unreleased

LLM-EdgeFlow 尚未正式发布。项目版本统一记录在 [CMakeLists.txt](../CMakeLists.txt) 的 `project(VERSION)` 中。
研发过程和迁移历史通过 Git 追溯；正式发布后按版本记录用户可感知的能力与契约变化。

当前能力：

- 移除无运行时消费者的生成版本头和产品版本宏，取消多份文档的版本号同步检查；
  依赖版本校验及 SDK 导出符号白名单继续保留。

- SDK 默认产物基名改为 `llm_edgeflow_sdk`，通过 `LLM_EDGEFLOW_SDK_OUTPUT_NAME` 集中配置；
  效果验证移除未被 Demo 加载的 SDK 文件指纹，继续记录实际执行的 Demo 指纹。见
  [构建扩展目录](dev_guide/source_layout.md#构建扩展目录)。

- 默认构建与开发门禁只启用 Inferlet；其他 Backend 通过开关或部署 preset 显式启用。
  引擎选择与互斥检查集中在 `BackendOptions.cmake`，原多引擎预设改名 `full-cpu`。
- `inferlet` Backend 和 `inferlet-cpu` 构建复用现有文本、图像生成和
  生成 token 向量协议；提供 [Inferlet 实体抽取样例](../configs/pipeline_entity_extract_inferlet.json)。
  依赖固定为 Inferlet v0.1.1 Linux CPU 静态包，现有 Kite 接入保持不变。
- 错误码收敛为中立框架码和平台对外码两套；内部取消裸码与旧异常码兼容参数，
  Studio/CLI 使用同一错误名称表。Operator 在失败出口记录分级日志与内外码，
  中间层保留具体诊断。见[错误码与日志](dev_guide/error_handling.md)。
- Operator 失败先在接入层共享代码按阶段和内部码判定 10 个类别，协议壳只把类别映射为平台码。
  `EDGEFLOW_OPERATOR_SHELL`（默认 `mock`）为每个构建选择一套壳及其测试驱动、错误码镜像清单和
  Demo 载体夹具。壳无关一致性套件按类别和清单验证六个入口与 `ResolveOperatorConfigIo`，
  并用违约自测证明检查有效；公共返回码、日志与业务结果不变。见
  [Operator 失败分类](dev_guide/error_handling.md#operator-失败分类)。

- 四层架构采用单向依赖，C++ Operator 是宿主调用入口。协议壳和平台 binding 负责载体与内存布局，
  Converter 负责完整请求和响应；内部类型不承担外部协议。见[架构与接入边界](architecture.md)。
- Pipeline 显式连接 typed 端口；Validator 生成执行计划，运行时消费计划。模型类别与 Backend
  协议选择唯一实现，文件相对 Pipeline 文档目录解析。见[配置说明](../configs/README.md)。
- 通用和自定义 Node 使用统一的函数与 Spec。失败保留原始原因、错误码及批次来源；
  参数更新使用原子快照，失败保持原值。会话模型注册保留身份校验和批量安装事务，
  缓存按会话、模型名和输入隔离。见[Node 作者指南](dev_guide/custom_node_concepts.md)。
- [文本 LLM Recipe](dev_guide/recipe_text_llm_node.md) 从 runner 的完整发现列表中筛选本节点用例，
  校验实际通过数；空筛选、测试失败或跳过不会被视为验证成功。
- 当前业务响应使用统一 JSON 输出载体；共享容量声明，保留按行关联、池预算和失败不发布输出。
  新类型与命名布局通过登记扩展。见[输出池与生命周期](dev_guide/operator_output_allocation.md)。
- 模型执行层提供通用 BPE 分词器 `text::BpeTokenizer`：读取开发者用 `tools/prepare_bpe_tokenizer.py`
  从 Hugging Face `tokenizer.json` 离线生成的 `.eftok` 文件，规范化、预分词、字节回退、合并优先级和解码器由文件头参数选择，
  与具体模型无关；工具对不支持的组件明确报错，`score` 优先级只在能证明与 HF 合并顺序等价时生成。
  见[BPE 分词器文件格式](dev_guide/bpe_tokenizer_format.md)。
- 原生 CLI 提供 Catalog、配置校验、计划与部署解析；Studio 使用同一构建的能力声明。
  HTTP 请求字段使用 `revision`、`filename`、`conf_name`。Demo 套件使用 `alg_demo --suite`。
  Studio 提供空态新建/导入入口，先选示例再创建，空白编排单独选择；可直接放大选中节点、
  查看输入输出设置及示例数据来源。运行入口与实际执行分开，应用修改和保存文件的状态明确；
  编辑 I/O 时保留当前构建未注册 Converter 的已有参数。
  Catalog 仅在页面启动时加载；编排直接调用原生 edit，删除重复 Profile 接口、旧路径入口和多份状态。
  保存操作串行核对 revision 并只替换 JSON；已关联 .conf 保持原文，删除双文件回滚流程。
  运行命令附件失败单独提示已保存状态。
  取消和超时由运行线程终止并回收进程组，异常日志保留末尾 2 MiB；自动 Profile 匹配跳过无关 I/O。
  见[Studio](../tools/pipeline_studio/README.md)和[统一 Demo](dev_guide/business_onboarding.md#5-统一-demo-接入)。

验证范围：默认开发门禁覆盖编译、契约与行为测试、SDK/Demo smoke、CLI/Studio 工具和架构守卫。
真实模型执行、浏览器交互、公司内部 SDK 及目标设备需要各自的资产、环境和验收证据；
默认门禁通过不替代这些验收。详见[验证范围与发布准备](VERIFIABLE_SELECTION.md#验收范围与发布准备)。
