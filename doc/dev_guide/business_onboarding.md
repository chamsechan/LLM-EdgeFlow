# 新业务接入：从平台结构到统一 Demo

本指南面向需要转换平台输入输出的 C++ 开发者。先按根目录
[README](../../README.md#快速开始)完成默认构建；以下命令都从仓库根目录执行。
公共结构或协议变更先按 [CONTRIBUTING](../../CONTRIBUTING.md)记录 RFC，再开始实现。

## 1. 先确定要走哪条路径

先运行 `./build/alg_pipeline_tool catalog`，核对已有业务契约、操作和模型能力。

| 需求 | 修改范围与下一步 |
| --- | --- |
| 外部结构不变，只调整规则、提示词、模型或连线 | 修改 Pipeline 和必要的 `.conf`，按[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)验证；复用已有 Adapter、bridge 和 Demo |
| 外部结构不变，但已有 Node 无法完成算法 | 按[自定义 Node 入门](first_custom_node.md)实现缺失算法，再复用已有接入路径 |
| 注册全新生产业务，接收新的外部结构 | 完成下文的契约、Adapter 和 Operator 步骤；需要统一 Demo 时再增加数据转换 |
| 修复已有 C ABI 的转换逻辑 | 修改对应 Adapter 并运行相关契约测试；仅影响该路径时，无需另建 Operator 或 Demo |

当前共享 SDK 的 Operator 初始化会审计**所有已注册 Adapter**。新增生产 Adapter
必须有匹配的 bridge，否则整个 Operator 初始化失败；`GetOperatorLastError()` 会指出业务与缺失 bridge、
不匹配类型或槽位原因。重复初始化保留首次冲突原因。目前没有仅注册 C ABI 业务的豁免模式。已有宿主类型可以复用其 ValueType 注册，全新类型才需要增加注册。

## 2. 用一个现有业务看清文件关系

以“输入一句文本，返回关键词匹配结果”为例。它使用现有规则节点，不需要模型权重。
先确认实际注册，再运行一次完整路径：

```bash
./build/alg_pipeline_tool catalog --biz keyword_match_v1
./build/alg_pipeline_tool describe-node TextRuleMatchNode
./build/alg_pipeline_tool validate configs/pipeline_keyword_match_rules.json
./build/alg_pipeline_tool plan configs/pipeline_keyword_match_rules.json
./build/alg_demo --biz keyword_match --config configs/pipeline_keyword_match_rules.conf --dataset tests/fixtures/effects/keyword_inputs.txt --no-default-control --output-dir results/business-onboarding
```

查看 `results/business-onboarding/keyword_match/results.jsonl`：请求编号为 20001–20004，
四条 `status` 均为 0，前两条 `output.is_hit` 为 true 且类别为 `SYSTEM_INIT`，后两条
为 false。`summary.json` 应有四条成功、零条失败。这一步用于认识已有接入链路；
新业务仍须换成自己的配置和输入验证。

按下表阅读对应代码，再在同一位置实现新契约；表中的名称都是这个已注册样例的名称。

| 环节 | 样例文件 | 新业务要落实的内容 |
| --- | --- | --- |
| 本地模拟平台结构 | [C ABI 类型](../../include/platform_mock/alg_types.h)、[Operator 数据结构](../../include/platform_mock/operator_data_types.h)、[平台交互类型](../../include/platform_mock/operator_types.h) | 业务类型、输入输出字段、长度和所有权；本目录只保存当前环境的模拟约定，真实公司定义在授权内网接入 |
| 内部数据边界 | [业务 key](../../include/adapter/biz_blackboard_keys.h)、[业务 Result](../../include/adapter/biz_results.h) | ingress/egress typed key 与接入适配层持有的结果值；已有类型可复用 |
| Adapter | [keyword_match_adapter.cpp](../../src/adapter/biz/keyword_match_adapter.cpp) | `GetDescriptor`、`Unpack`、`PackTyped` 和 `REGISTER_BIZ_ADAPTER` |
| Operator 类型注册 | [operator_builtin_value_types.cpp](../../src/adapter/operator/operator_builtin_value_types.cpp) | 为新宿主类型登记规范后缀、输入校验或输出分配/重置/释放 |
| Operator 业务桥接 | [keyword_match_operator_bridge.cpp](../../src/adapter/biz/keyword_match_operator_bridge.cpp) | 两端结构的字段转换、输入输出槽及 `REGISTER_OPERATOR_BIZ_BRIDGE` |
| Demo 数据转换 | [keyword_match_demo.cpp](../../demo/biz/keyword_match_demo.cpp) | 读数据集、构造平台输入、复制输出字段及 `REGISTER_DEMO_BIZ` |
| 构建与部署 | [接入适配层 CMake](../../src/adapter/CMakeLists.txt)、[Demo CMake](../../demo/CMakeLists.txt)、[Pipeline](../../configs/pipeline_keyword_match_rules.json)、[部署配置](../../configs/pipeline_keyword_match_rules.conf) | 登记新增 `.cpp`，编排业务端口，配置路径和输出容量 |

其中，Pipeline 的 `biz_name`（`keyword_match_v1`）、Demo 的 `--biz`（`keyword_match`）
和 Operator 槽位后缀（`keyword_in` / `keyword_out`）用途不同。
前两者的关联由 Adapter 的 `BizDefinition` 声明；槽位由 bridge 绑定到已注册宿主类型。
新名字必须分别登记，不能只改 JSON 中的显示名称。

## 3. 实现并注册 Adapter

新增 Adapter 放在 `src/adapter/biz/<biz>_adapter.cpp`，参照上面的关键词实现完成：

1. **声明外部契约。** 当前本地示例的 C 数据结构放在 `platform_mock/alg_types.h`，
   使用 C11 类型，明确批次上限、指针有效期和输出容量；函数入口仍在 `edgeflow/c_api.h`。
   在 `biz_blackboard_keys.h` 复用或增加 typed key；Node 使用中性 Batch 和逻辑端口，
   不包含平台结构或业务 key 头。
2. **填写描述符。** `GetDescriptor()` 返回业务枚举、结构名、所有权与批次约束；
   `AdapterDescriptor::biz_definitions` 中的 `BizDefinition` 声明允许的 `biz_name`、Demo 名和
   ingress/egress。配置里的业务名必须与这里一致。
3. **实现输入转换。** `Unpack` 使用 `AdapterValidationHelper` 检查批次、指针和长度，
   将输入复制到本次 `AlgContext`。关键词样例把外部请求编号保存在 `raw_request_ids`，
   Batch 的 `req_id` 使用批内编号；输出阶段必须按来源映射回原编号。
4. **实现一次结果打包。** 继承 `ResultPackingAdapter<Adapter, COutput, Result>`，
   在 `PackTyped<Output>` 中检查结果完整性、按来源关联结果并映射字段。
   `COutput` 是公共 C 输出；`Result` 放在 `biz_results.h`，字符串由它自己持有。
   字符串字段使用 `CopyResultString`：C 数组容量不足时报错，Result 则保存完整内容。
5. **登记并编译。** 文件末尾使用 `REGISTER_BIZ_ADAPTER`，将源码加入
   `src/adapter/CMakeLists.txt` 的 `edgeflow_integration_objects`。新业务的 Operator
   bridge 也要完成后再验证 SDK 初始化，不修改 `c_api_adapter.cpp` 的中央分发。

多路内部输入/输出的打包可参考
[doc_qa_adapter.cpp](../../src/adapter/biz/doc_qa_adapter.cpp)。平台边界检查的独立
练习见 [Adapter 安全示例](adapter_templates/README.md)，它们不注册生产业务。

## 4. 补齐 Operator 类型和业务桥接

**ValueType 说明“这块平台内存是什么类型、如何检查和管理”，bridge 说明“这个业务如何转换它”。**
按这个顺序实现：

1. 当前环境的模拟宿主结构先在 `platform_mock/operator_data_types.h` 声明，再在
   `operator_builtin_value_types.cpp` 中通过 `RegisterBinding` 登记。
   输入 binding 指定规范后缀、外部类型、I/O 方向和校验函数；输出 binding 还需声明
   每个字符串的默认/最大容量、metadata 上限、池载荷预算及分配、重置、释放行为。
   同文件的 `MakeTypedInputBinding` / `MakePooledOutputBinding` 是现有类型的实现参考。
   公司内部公共头的接入遵循 [平台模拟定义的迁移边界](../../include/platform_mock/README.md)。
2. 新建 `src/adapter/biz/<biz>_operator_bridge.cpp`。单输入/单输出时，沿用
   `MakeSingleSlotBizBridge<Result>`，填写与 Adapter 一致的业务类型和结果类型，指定
   已注册的输入输出后缀；多输入时参考
   [ocr_doc_qa_operator_bridge.cpp](../../src/adapter/biz/ocr_doc_qa_operator_bridge.cpp)。
3. 实现 `convert_sample_input`：从输入槽读取宿主结构，使用
   `ProcessLocalShadowStorage` 复制字符串、保存临时输入结构。实现
   `convert_sample_output`：把业务 Result 复制进已租用的输出池，容量只取自
   `ResolvedOutputPoolSpec`，不在 bridge 内重新读取原始 JSON 或设置默认容量。
4. 包含 `adapter/operator_biz_bridge.h`，使用无参注册函数调用
   `RegisterOperatorBizBridge(desc)`；输出文本使用 `CopyToOperatorString`。
   用 `REGISTER_OPERATOR_BIZ_BRIDGE` 登记注册函数，并将新增源码加入
   `src/adapter/CMakeLists.txt`。宿主传入的命名 I/O Key 后缀必须与注册后缀精确一致。

`CompanyString` 用于文本，二进制使用 `CompanyBuffer`。输入借用指针不跨调用保存；
输出提取时复制所需数据，具体生命周期按
[Operator 接口](../../include/edgeflow/operator/interface.h)执行。
上述转换都留在接入适配层，Node、Model 和 Backend 无需识别宿主结构。

## 统一 Demo 接入

已有契约的新方案直接沿用对应 Demo 和数据集格式，只准备 Pipeline 与指向它的 `.conf`。
新增外部结构需要统一 Demo 时，按 `keyword_match_demo.cpp` 完成以下步骤：

1. 新建 `demo/biz/<biz>_demo.cpp`，从数据集读入样本，为每条样本构造宿主输入结构，
   保持字符串及数组在同步处理期间有效。
2. 使用 `RunOperatorWithExtractor<Input, Output>`，传入 bridge 声明的槽位后缀；
   在 extractor 中把输出复制到本地结果值，再交给 `ResultWriter` 输出逐条记录。
   同时复制真实 `status_code`，写入样本的 `status`，不能固定填零。Process 返回成功表示
   调用完成，业务是否逐条成功还需检查 `results.jsonl` 和 `summary.json`。
   执行、参数解析和输出池管理继续复用运行器。
3. 用 `REGISTER_DEMO_BIZ(name, title, run_function, biz_type)` 注册，`name` 与
   `BizDefinition` 的 Demo 名一致，业务类型显式给出且非 UNKNOWN；将源码加入
   `demo/CMakeLists.txt`。无需在 `demo/main.cpp` 增加业务分支。
4. 准备样例数据、Pipeline 和 `.conf`。先用显式 `--biz`、`--config`、`--dataset` 运行。
   仅需保存可重复调用的预设或加入套件时，再向 `demo/profiles.json` 添加 Profile。

`.conf` 的 `data.pipe_path` 相对部署根解析。像本页这样从仓库根目录传入相对
`--config configs/pipeline_keyword_match_rules.conf` 时，填写
`configs/pipeline_keyword_match_rules.json`。宿主直接调用 Operator 时，部署根为 Create 的
`model_path`；同时核对 `data.model_paths` 覆盖与输出容量。Profile 不会自动指向新方案，
详细命令见[运行当前方案](../../tools/pipeline_studio/README.md#运行当前方案)。

## 输出容量

宿主输入是借用视图，底层字符串、数组和结构体必须保持有效直到 `Process` 返回。
输出 `shared_ptr<void>` 持有的是当前 handle 的池租约，不延长 handle 的生命期。
需要保存结果时，在本次调用后复制到自己的 `std::string` / 值对象，再清空输出容器。
不要累积所有输出租约后在同一线程继续同步 `Process`：池满时调用会等待空闲块，
该线程也就无法返回释放旧租约。池深用于控制同时持有的输出数量，不是结果存储空间。

销毁顺序是：等待所有 `Process` / `Control` 返回 → 释放输出引用 → `Destroy`。
有效 handle 即使因未归还输出而在 `Destroy` 返回错误，也已被消费，不得重试或再访问
旧输出。参考 [Demo 的输出复制与释放](../../demo/biz/ocr_doc_qa_demo.cpp) 和
[公开 Operator 契约](../../include/edgeflow/operator/interface.h)。

Operator 的输出路径是 `Pipeline → 可变长业务 Result → 已租用输出池`。
Result 与请求 Context 均不跨 Process 保存。`.conf` 的 `data.mem_que.type` 选择已注册
输出类型，`capacities` 设置它声明的字段容量。字符串不受中间 C 输出数组大小限制；
超过输出池容量时返回 `-4`，尚未发布的输出租约全部回滚。

公共 C ABI 继续使用已发布的固定数组大小。需要大结果时选择 Operator，或通过新的
外部 ABI 版本扩展；不改变旧结构布局。

## 最小验证

完成源码登记后重新构建，再检查新业务是否进入 Catalog：

```bash
cmake --build build --target alg_sdk alg_pipeline_tool alg_demo -j 4
./build/alg_pipeline_tool catalog
```

确认 Catalog 中出现新 `biz_name`，ingress/egress 类型与 Adapter 一致；随后对**本次新增或
修改的 Pipeline** 执行 `validate`、`plan`，运行对应 Demo 并核对请求 ID、状态及业务字段。
第 2 节的关键词命令是可运行参照，实际验证时替换为新业务、配置和数据集。
有意使用测试模型时按[工具选择](../../tools/pipeline_studio/README.md#校验工具选择)
构建并使用 `alg_pipeline_tool_test`。

把断言加入相应现有套件：

| 验证范围 | 必须观察到的行为 | 参考测试 |
| --- | --- | --- |
| C ABI / Adapter | 非法指针和长度被拒绝；结果乱序仍按来源返回，重复/缺失来源与失败结果被拒绝；两种输出表示一致 | [Adapter 契约测试](../../tests/contract/abi/test_adapter_contract_security.cpp)、[C ABI 测试](../../tests/contract/abi/test_c_abi_safety.cpp) |
| Operator | 初始化接受完整注册；超过旧 C 数组但在池容量内时输出完整，超池容量时无部分发布且后续请求可复用租约 | [bridge 测试](../../tests/unit/operator/test_operator_biz_bridge_registry.cpp)、[Operator 集成测试](../../tests/integration/operator/test_operator_api.cpp) |
| Pipeline / Demo | 新业务通过校验和计划，样例结果及错误路径符合预期 | [Catalog/Validator 测试](../../tests/integration/pipeline/test_pipeline_catalog_validator.cpp)、[Demo 测试](../../tests/integration/demo/test_demo_runner.cpp) |

仅修改 C ABI 路径时，使用对应端到端契约测试验收；统一 Demo 走 Operator。
交付前执行 `./scripts/run_all_tests.sh`。真实模型效果与目标平台验收按
[效果验收指南](../VERIFIABLE_SELECTION.md)另行记录；涉及公司内部 SDK 时遵循
[RFC-0029](../rfcs/0029-external-readiness-and-intranet-sdk-migration.md)，当前外部工作区只准备中立接口。
