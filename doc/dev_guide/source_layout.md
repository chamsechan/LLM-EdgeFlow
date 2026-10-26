# 源码布局与命名

文件按职责归属，头文件按使用者范围放置。架构职责与依赖方向见
[架构设计](../architecture.md)，四层职责名称、源码目录与构建目标的对照见其中的
[架构总览](../architecture.md#1-架构总览)。

## 仓库顶层目录

| 目录 | 内容 |
| --- | --- |
| `include/` | 头文件，按[使用范围](#头文件的三种使用范围)分目录；`contracts/` 是各层共用的轻量运行时契约（`edgeflow_runtime_contracts`），`platform_mock/` 是平台公共定义替身 |
| `src/` | 四层实现：`adapter/`、`core/`、`common_nodes/` 与 `custom_nodes/`、`engine/`；`cli/` 是随框架编译的命令行工具 `alg_pipeline_tool` 与 `alg_show` |
| `demo/` | 统一 Demo 程序、Demo Profile 与 Mock 方案（`fixtures/mock/`） |
| `configs/` | 示例方案、部署 `.conf` 与模型资产清单（权重不入库），命名见[配置说明](../../configs/README.md) |
| `data/` | Demo Profile 使用的示例数据集 |
| `dev_support/` | 测试用 Model/Backend 替身、Node 起步模板和可选基准，不链接进生产 SDK |
| `tests/` | 按 `unit/`、`integration/`、`contract/`、`e2e/` 分类的测试，见[测试指南](../../tests/README.md) |
| `tools/` | 开发者直接运行的工具：Pipeline Studio、Node 脚手架、开发 Recipe、模型选择检查与 BPE 分词器生成工具（构建时只调用其 `fixture` 子命令生成测试用合成文件） |
| `scripts/` | 构建、门禁、测试、格式化、架构图渲染与交付脚本 |
| `cmake_ext/` | 见[构建扩展目录](#构建扩展目录) |
| `doc/` | 架构、开发指南与参考文档，入口见[文档目录](../README.md) |
| `plans/` | 跨多个 PR、仍在进行中的工作计划；不是现行规则，最后一个阶段合入后删除，见[工作计划](../../plans/README.md) |
| `.agents/skills/` | 项目开发 Skills，用法见[文档目录](../README.md#使用开发-skills) |

新增工具时，按用途选择目录：

- 开发者手动运行的工具放在 `tools/`。
- 构建、门禁和 CI 脚本放在 `scripts/`。
- 需要链接框架运行时的 C++ 命令行程序放在 `src/cli/`。可执行文件输出到 `build/`。

## 构建扩展目录

本项目的 CMake 模块、生成模板和 Node 契约清单位于 `cmake_ext/`。
顶层 `CMakeLists.txt` 和测试引用这些文件。根目录的 `cmake/` 留给公司内部构建系统。
`cmake` 命令、`CMakeLists.txt` 文件名，以及第三方安装包的 `lib/cmake/` 路径保持原约定。

根 `CMakeLists.txt` 只负责禁止源码内构建、声明项目和按顺序加载模块：

| 模块 | 职责 |
| :--- | :--- |
| `PlatformOptions.cmake` | 解析目标 OS/架构，校验 preset 和交叉编译执行边界；在依赖获取之前拒绝不支持的组合 |
| `BackendOptions.cmake` | Backend 开关、平台限制与互斥检查；Linux 默认 Inferlet，macOS 默认 llama.cpp CPU |
| `ProjectSetup.cmake` | 集中编译选项、第三方静态依赖策略与 SDK 导出规则，选择 Operator 壳，创建分层头视图及 public/extension/internal 头目标 |
| `Dependencies.cmake` | 聚合项目依赖；每个依赖独立维护 |
| `RuntimeGraph.cmake` | 各层依赖与 OBJECT 目标、Backend 能力接线，加载 `src/` |
| `ProductTargets.cmake` | SDK 产物、输出路径、Demo Profile 字段头、Demo、CLI 和测试入口 |

`cmake_ext/` 根目录只保留上述六个入口，其余文件按职责归类：

| 子目录 | 内容 |
| :--- | :--- |
| `engines/` | 推理运行时的依赖接入模块 |
| `third_party/` | JSON、PCRE2、stb_image、GoogleTest 等通用依赖，以及引擎和通用依赖共用的校验下载、缓存身份与归档发布工具 |
| `support/` | 构建加速、Operator 壳选择和分层头文件视图 |
| `toolchains/` | 在 `project()` 之前生效的编译器、sysroot 和搜索规则；当前提供中性的 GNU/Linux arm64 示例 |
| `templates/` | SDK 导出符号脚本、Demo Profile 字段头等生成文件的输入模板 |

平台配置顺序是 **Preset/Toolchain → `project()` → 平台校验 → Backend 选择 → 编译策略 →
依赖 → 组件目标**。Preset 选择场景，Toolchain 定义目标环境；`PlatformOptions.cmake` 只读取
CMake 的目标信息，不根据宿主 CPU 猜测目标包。Apple 的显式 `CMAKE_OSX_ARCHITECTURES`
优先于 `CMAKE_SYSTEM_PROCESSOR`，每个目录只构建一种架构。组件 `src/**/CMakeLists.txt`
继续负责源码归属和依赖，新增平台应优先修改入口策略，避免向组件编译规则散布平台判断。

ELF 和 Mach-O 的导出限制从同一份
[`edgeflow_sdk.exports`](../../cmake_ext/templates/edgeflow_sdk.exports) 生成；导出检查保留独立的公开 ABI 预期。
具体平台入口及验收范围见[跨平台构建](../VERIFIABLE_SELECTION.md#跨平台构建)。

SDK 产物基名统一由 `LLM_EDGEFLOW_SDK_OUTPUT_NAME` 指定，默认 `llm_edgeflow_sdk`。
迁移到宿主构建时可传入 `-DLLM_EDGEFLOW_SDK_OUTPUT_NAME=host_algorithm`；填写基名，
不包含目录或平台的 `lib` 前缀、`.so` / `.dylib` / `.dll` 后缀。
CMake 链接方继续使用 `llm_edgeflow::sdk`，脚本和测试通过目标路径定位产物。
修改名称后应重新构建并更新宿主的加载路径；旧构建目录中遗留的旧名库不会自动删除。

推理引擎的依赖接入统一放在 `engines/`，包括 ONNX Runtime、llama.cpp、Whisper、Inferlet 和 Kite。
目录按运行时职责划分，不按作者、仓库公开性或接口来源划分；各模块只维护固定版本、上游选项
和编译链接探针。校验下载、发布包导入、缓存身份与归档发布由
`third_party/ThirdPartyCacheMetadata.cmake` 共用。引擎模块把 Backend 的 SYSTEM 头目录、编译定义和
链接目标分别追加到 `LLM_EDGEFLOW_BACKEND_INCLUDE_DIRS`、`LLM_EDGEFLOW_BACKEND_COMPILE_DEFINITIONS`、
`LLM_EDGEFLOW_BACKEND_LIBRARIES`，由 `RuntimeGraph.cmake` 统一接入 Backend 实现；新增 Backend 不修改它。
Whisper 复用 llama.cpp 的 GGML，因此依赖聚合中先加载 llama.cpp。
测试依赖 GoogleTest 在测试入口按需加载。根文件负责装配。

当前构建使用 CMake 3.19、预设和现有依赖来源。尚未接入公司 `platform` / `sdk`、
toolchain、`DH_*` 或镜像规则；这些宿主构建约定须在授权内网确认，不能由引擎模块推定。
仅检查组织边界和模块引用可运行：

```bash
cmake -P tests/contract/architecture/test_cmake_structure.cmake
```

该检查无需编译器、网络或 SDK，不证明真实编译、链接和公司 SDK 布局正确。
完整构建与测试仍使用 `./scripts/run_all_tests.sh`。

`src/` 各层实现目录、`src/cli/` 与 `demo/` 使用 `GLOB_RECURSE CONFIGURE_DEPENDS`
递归收集 `.cpp`，增删普通实现文件后，下次构建自动更新文件列表。
Engine 的 `backends/` 单独归 Backend 目标。Adapter 的 `shared_algorithm_runtime.cpp`
与本地日志等级实现 `src/log.cpp` 显式归组合根。CLI 的两个可执行入口及 Demo 的 `main.cpp` 从公共实现中排除，
分别归自己的可执行目标。新增独立目标时仍须声明其入口和归属，不能混入已有目标。
测试按 `tests/RuntimeTests.cmake` 的目录和命名规则收集。跨 runner、进程隔离、生成源码及
可选 E2E 目标保留显式登记。常规 GoogleTest 用例自动发现并生成 CTest 入口。

## 头文件的三种使用范围

| 使用者 | 位置与构建目标 | 约定 |
| --- | --- | --- |
| SDK 调用方 | `include/edgeflow/`；`edgeflow_public_headers` | Operator 和日志接口；只有明确列举的调用头向 SDK 消费方传播 |
| 源码扩展开发者 | `include/adapter/`、`include/core/`、`include/nodes/`、`include/engine/`、`include/contracts/`；`edgeflow_extension_headers` | Adapter、Node、Model、Backend 的源码接口与共享契约，需要随框架重新编译，不承诺内部 C++ 动态 ABI |
| 模块实现与仓库测试 | `src/` 中与 `.cpp` 相邻；`edgeflow_internal_headers` | 运行时装配、配置解析、注册表内部及输出池等实现 |

运行时四层仍使用各自受限的 include view，不链接覆盖整个仓库的内部头目标。
模板、内联函数和扩展基类可以直接实现在头文件中。是否需要头文件由使用范围决定。
多个 `.cpp` 或测试需要引用一个声明，不意味着它应成为公开 SDK 接口。

使用范围之外，还要区分定义的来源：`include/platform_mock/` 存放当前外网环境使用的
平台公共定义替身，包括业务 DTO、平台枚举、控制参数、命名 I/O、函数表类型和错误码。
它们作为现行调用接口的依赖，被显式列入 SDK 头视图，但不属于框架自有数据模型。
具体清单与排除项见[平台模拟定义](../../include/platform_mock/README.md)。

## 接入适配层

```text
include/edgeflow/                 SDK 调用接口
  export.h / log.h
  operator/interface.h
  operator/types.h                Mock* 数据 DTO 门面
include/platform_mock/            本地平台公共定义模拟
  log.h                          日志等级与简化打印宏，由 edgeflow/log.h 转发
  error_codes.h
  operator_data_types.h / operator_types.h
include/adapter/                  源码扩展契约与辅助接口
  io_converter.h
  io_converter_registry.h
  operator_value_type.h             中性值与平台 binding 接口
  io_values.h                      Converter 内容值
  operator/mock/platform_value_binding.h  当前模拟平台的布局 helper
src/adapter/
  shared_algorithm_runtime.cpp/.h
  deployment_io_config.cpp/.h
  io_plan_resolver.cpp/.h
  input/                          各业务输入转换器
  output/                         各业务输出转换器
  operator/                       Operator 通用机制；子目录只存放具体协议壳
    operator_config_resolver.cpp/.h
    operator_failure_category.cpp/.h  失败阶段与类别判定，不含平台头
    operator_output_pool.cpp/.h
    operator_value_type_registry.cpp/.h
    mock/                         本地模拟平台协议实现
      operator_process_binding.cpp/.h
      operator_adapter.cpp
      operator_error_mapping.cpp/.h  类别到平台码的映射表
      platform_value_binding.cpp  当前模拟平台的校验、分配、预算与重置
```

### Operator 壳选择

CMake 缓存变量 `EDGEFLOW_OPERATOR_SHELL`（默认 `mock`）为每个构建选择一套协议壳及其配套
夹具，由 `cmake_ext/support/OperatorShell.cmake` 统一处理：

| 内容 | 位置 |
| --- | --- |
| 壳源码 | `src/adapter/operator/<shell>/`，以及可选的 `include/adapter/operator/<shell>/` |
| 测试驱动与错误码镜像清单 | `tests/contract/operator_shell/<shell>/`，只进入测试目标 |
| Demo 载体构造 | `demo/operator_shell/<shell>/` |
| 依赖该壳载体的测试 | 所属测试目录下的 `operator_shell/<shell>/` |

以上三个必需目录缺一时配置失败。Adapter、Demo、测试 runner 和集成层头视图的通配收集
先排除全部具体壳及 `operator_shell/<shell>/` 夹具目录，再加入选中项。壳无关一致性套件位于
`tests/contract/operator_shell/` 本层。公共头视图和平台声明仍使用当前来源；该机制只隔离源码与
夹具，不表示已经可以切换真实平台头。`OperatorShellSelectionTest` 检查默认选择、缺失目录失败、
未选中壳的排除，以及生产目标不含测试驱动和清单。

编写转换器时，包含 `adapter/io_converter.h` 和 `adapter/converter_authoring.h`。
实现 InputConverter 或 OutputConverter 回调后，声明对应 Definition。
用 `REGISTER_INPUT_CONVERTER` 或 `REGISTER_OUTPUT_CONVERTER` 注册转换器。
常见单槽、每请求一行的回调使用 `DecodeRequestRows` / `EncodeResultRows` 调用普通业务函数，
批次与绑定归辅助层。多槽、展开和汇聚保留显式算法。
每个转换器登记一个宿主槽、类型端口与共享参数声明。Pipeline 根 `io` 选择这些登记。
端口 Definition 与回调共用同一类型端口常量，端口名即业务键名，
绑定不做改名。常见必需槽可用 `ExternalInputSlot<T>()` /
`ExternalOutputSlot<T>()` 声明中立值类型。登记的 `type` 选择 binding，输出容量字段由已注册 ValueType 决定。
特殊布局仍使用完整定义。
业务专属实现可按修改关联同文件组织，共享转换器保留独立引用。不要求为每个业务创建聚合宏或新注册表。
宿主值类型与命名输出分配方案通过 `adapter/operator_value_type.h` 登记。实现只管理
单份结构及嵌套存储，队列、租约和初始化审计归通用机制所有。常见类型直接使用
`MakeTypedInputBinding<T>` 与 `MakePooledOutputBinding<T>`。模板保留在扩展头中，
非模板分配实现归 `src/adapter/operator/`，不按业务复制池机制。
完整步骤见[业务接入](business_onboarding.md)。

## 流程编排层

```text
include/core/                     编排契约与 Node 注册接口
  pipeline_config.h               Pipeline JSON 解析结果
  pipeline_validator.h            依赖推导与校验，产出 ValidatedPipelinePlan
  pipeline.h                      按校验计划执行
  alg_context.h / blackboard_key.h / session_context.h
  node_interface.h / node_definition.h / node_registry.h / port_definition.h
  pipeline_catalog.h
src/core/                         上述接口的实现；pipeline_config_structure.cpp/.h 等私有头相邻放置
```

## 能力节点层

```text
include/nodes/                    Node 作者接口，模板与内联实现，没有对应的 src/nodes/
  authoring.h                     Node 作者统一包含的入口头
  function_node.h                 Spec 声明、AuthorNode 与 REGISTER_FUNCTION_NODE
  node_base.h                     Node 运行时基类 NodeBase
  model_binding.h / model_calls.h / control_authoring.h
  traceable_batch_operations.h    Join、Group 等批处理与来源追踪辅助
include/contracts/parameters.h    四层共享的参数声明与解析
src/common_nodes/                 框架维护的中性 Node，每个文件一个 *_node.cpp
  support/                        多个 Node 共用的私有辅助
src/custom_nodes/                 领域算法 Node，按操作而非业务命名
```

`tools/scaffold_custom_node.py` 按同一结构生成单输入单输出 Node 到 `src/custom_nodes/`；
多输入、多模型和批次示例在 `dev_support/node_authoring/`。见[自定义 Node 源码指南](../../src/custom_nodes/README.md)。

## 模型执行层

```text
include/engine/                   Model/Backend 接口、注册表与 FixedBatchExecutor
src/engine/
  runtime/                        Model/Backend 注册表与运行时工厂
  models/<模型>/                  模型预处理与语义；共享辅助放在 common/、bge_common/
  backends/<运行时>/              厂商运行时资源，厂商头文件只在此处包含
  text/ / text_generation/        UTF-8 处理与通用自回归生成
```

## 标识符与定义

| 名称 | 含义 |
| --- | --- |
| Converter `(type, name)` | 按方向唯一；type 是宿主 key 后缀，name 对应业务值 |
| `NodePortDefinition.logical_name` | Node 的逻辑端口名称，由 Pipeline 映射到具体黑板键 |
| `IoPortDefinition.blackboard_key` | 输入/输出边界使用的实际黑板键 |

配置在根 `io.input` / `io.output` 的每项填写 `type`、`name` 和可选 `params`。
框架按方向和这一对标识查找登记，由所选端口组成 Core 边界。
宿主 key 固定为 `<prefix>.<type>`，prefix 由宿主生成、非空且不含 `.`；同一方向内每种 type 至多选用一次。
业务 name 使用 `snake_case`，描述外部请求/响应的语义，例如 `ocr_invoice_qa`。
方案、数据集与 Profile 可以沿用对应词根和部署变体。它们不定义新载体类型或转换器身份。

Demo 按宿主载体组织在所选壳夹具的 `demo/operator_shell/<shell>/input/`、`output/`，
文件名描述载体及展示，由 `REGISTER_DEMO_INPUT` / `REGISTER_DEMO_OUTPUT` 登记。
同一载体上的业务共享这些代码；与载体无关的运行、数据集读取和结果写入留在 `demo/common/`。

转换器由宿主后缀和业务名配对标识，不带版本号：发布前直接改名，发布后的不兼容变化见
[CONTRIBUTING](../../CONTRIBUTING.md#3-design-and-current-contracts)。

宿主槽在所属转换器 `.cpp` 内声明，回调通过选中项的 options 访问本地逻辑端口与不可变参数。

Node 与 Converter 的本地逻辑端口共用 `NodePortDefinition`，通过
`RequiredInputPort`、`OptionalInputPort`、`OutputPort` 声明。接入准备将选中端口与实际引用
组合成 `IoPortDefinition` 边界，交给 Core 验证。
Catalog JSON 的 Node 与 Converter 端口 `key` 都是本地逻辑端口名。
配置中的实际来源写为 `input.端口名` 或 `节点名.端口名`，由 Validator 形成类型化黑板绑定。

`core/port_definition.h`、`core/node_definition.h` 分别维护端口与 Node 元数据。
Converter 定义位于 `adapter/io_converter.h`，Catalog 查询位于 `core/pipeline_catalog.h` 和接入层 `IoCatalog`。
`engine/inference_definition.h` 维护 Model/Backend 元数据。张量与 Host 内存辅助接口
在 `engine/tensor.h`。`node_registry.h` 的主要类型是 `NodeRegistry`。

## 公共头与统一入口

SDK 调用代码使用以下 `edgeflow/` 前缀入口：

| 规范入口 | 说明 |
| --- | --- |
| `edgeflow/export.h` | 符号可见性宏 |
| `edgeflow/log.h` | 统一日志转发入口，当前使用 `platform_mock/log.h` 开发替身 |
| `edgeflow/operator/interface.h` | C++ Operator 接口及函数表 |
| `edgeflow/operator/types.h` | `Mock*` 数据 DTO 门面（转发至 `platform_mock/operator_data_types.h`） |

当前开发构建的 `Mock*` 类型、公共宏、C++ Operator 公开函数签名、结构布局及 SDK 产物名称
共同构成该构建的调用契约，不代表真实公司 SDK 的定义。内部扩展统一使用 `NodeRegistry`。

`edgeflow/operator/types.h` 仅转发 `platform_mock/operator_data_types.h` 中的 `Mock*`
数据 DTO。计算平台、Control 参数、命名 I/O 与函数表类型定义在
`platform_mock/operator_types.h`，由 `edgeflow/operator/interface.h` 引入。错误码来自
`platform_mock/error_codes.h`。
真实公司公共头需要在授权内网单独核对和接入。

示例配置和 Profile 的命名见[配置说明](../../configs/README.md)。
