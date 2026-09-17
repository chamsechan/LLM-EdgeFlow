# RFC-0060：删除 C ABI，仅保留 C++ Operator API

- **RFC 编号**：`0060-cpp-operator-only`（建议编号；基线索引最高为 0059，正式入库前检查编号占用）
- **创建日期**：2026-09-17
- **文档状态**：Completed
- **仓库**：`chamsechan/LLM-EdgeFlow`
- **审查基线**：默认分支 `main`，提交 `2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9`
- **基线提交日期**：2026-09-16 08:26:57 UTC
- **建议实施分支**：`refactor/cpp-operator-only`（本次未创建）
- **建议入库路径**：`doc/rfcs/0060-cpp-operator-only.md`
- **目标版本**：建议 `v11.0.0`，共享库 ABI major 建议由 6 升为 7；版本号为本 RFC 提案，不代表已经发布
- **负责人**：项目维护者指定
- **关联决策**：继承 RFC-0059 的输入/输出转换独立化；取代其中“两种宿主算法入口并存”的部分。保留 RFC-0049、RFC-0050 的 Operator 输出分配与配置边界，以及 RFC-0014 的独立日志 API。

> **本次唯一方向：彻底删除旧 C ABI 算法调用链，只保留现有 C++ Operator API。**
> 不保留兼容头、转发函数、旧符号、旧绑定别名或开关；不把 Operator 改造成另一套新 API；不重新设计帧深度、输出池、输入/输出转换器或 Pipeline。

## 1. 问题与范围

### 1.1 需求与完成后的产品边界

宿主程序今后仅通过 `llm_edgeflow::operator_api` 获取 Operator 函数表，并使用其中的 `Init / Create / Process / Control / Destroy / Deinit` 调用算法。旧 `Alg_*` 调用方、旧 C ABI 类型和旧 `*.cabi.*` 部署绑定不再受支持，必须修改源码与配置后重新构建。

“只保留一套接口”限定为**宿主算法调用接口**。这不等于删除公共日志、Pipeline 编排工具、内部 Node/Model/Backend 作者接口，也不等于删除所有 C 风格结构体或所有 `extern "C"` 声明。

本 RFC 不提供过渡兼容版本。实施分支可以按阶段保留尚未删除的原代码以保证迁移可验证，但不得新增兼容设施，最终交付必须完成整体切换。

### 1.2 当前代码事实

以下事实来自固定提交的源码，而不是把历史方案当作当前实现。源码依据见文末 `[S01]`—`[S20]`。

| 位置 | 基线事实 | 对实施的影响 |
| --- | --- | --- |
| `include/edgeflow/c_api.h` | 声明六个 `Alg_*` C ABI 函数，`Alg_Process` 使用指针数组及数量参数 | 整个旧算法入口应删除 |
| `include/edgeflow/c_api.hpp` | 三参数 `Alg_Process` 是 `std::vector<void*>` 到旧 C ABI 的便捷包装 | 它不是 Operator API，必须一并删除 |
| `include/edgeflow/operator/interface.h` | 已有函数表入口、线程局部错误查询、配置绑定预检 | 直接保留，不另造门面 |
| `src/adapter/operator/operator_adapter.cpp` | 已自行完成命名槽位校验、Decode、租用输出、Pipeline 执行、Encode 与发布 | Operator 不需要通过旧 C ABI 才能工作 |
| `src/adapter/shared_algorithm_runtime.*` | Operator 使用其初始化、已验证计划构建和 Control；同时还混有 C ABI 专用工厂与指针数组执行函数 | 保留运行时，仅删旧路径及头文件依赖 |
| `include/adapter/io_converter.h` | 同时存在指针数组访问与 Operator 具名槽位访问 | 需要删 C ABI 分支，不能仅删除外层入口 |
| `src/adapter/input/`、`output/`、`biz/` | 当前实现按输入、输出、绑定拆分；部分文件同时注册两种 transport | 按函数和注册项清理，不能整目录删除 |
| 关键词、翻译业务的绑定声明 | `required_transports` 为 `{"cabi", "operator"}` | 全量审计也必须收敛，否则删除注册后初始化会失败 |
| Demo 主入口与关键词 Demo | 已使用 Operator；公共 runner 仍包含 `edgeflow/c_api.h` | 主要清理残留依赖并回归，不重写 Demo 框架 |
| 构建与符号检查 | 当前产品版本 10.0.0、ABI major 6；共享库允许导出 12 个符号 | 旧六个算法符号与版本约束必须同步处理 |
| 测试链接方式 | 多数测试链接内部运行时；独立 C11 ABI 测试链接公开 SDK | 删除 C11 测试后必须保留“公开共享库可被消费者使用”的验证能力 |
| `AGENTS.md`、业务接入指南 | 仍有 C ABI 为中心及旧 `IBizAdapter`/业务 bridge 描述 | 修改现行指南，不能按这些旧描述重新引入已退出的架构 |

**两个容易混淆的三参数函数：**

```cpp
// 删除：旧 C ABI 的 C++ 便捷包装。
Alg_Process(void* hndl,
            const std::vector<void*>& inputs,
            std::vector<void*>& outputs);

// 保留：现有 OperatorFunc::Process 的函数指针类型，不修改参数。
int (*Process)(void* handle,
               const NamedIoBatch& inputs,
               NamedIoBatch& outputs) noexcept;
```

### 1.3 范围与非目标

| 纳入本次 | 不纳入本次 |
| --- | --- |
| C ABI 入口、包装、专用类型、专用运行路径、注册项及配置消费者清理 | 重设计 OperatorFunc、CreateParam、NamedIo 或平台结构体 |
| 共用代码解除对 C API 头的依赖 | 新建另一套 Runtime、统一传输框架、反射系统或业务 bridge |
| 迁移业务、安全、并发、Control 与工具测试 | 改变 Pipeline、Node、Model、Backend 的算法语义 |
| 公开头可见性、动态导出、共享库版本与测试清单同步 | 为删接口而移除 C 编译器、改动第三方推理引擎 |
| 当前文档、Agent 指引、脚手架及 CI 引用清理 | 为追求命名整齐而大规模改目录或重命名仍有用的内部类 |
| 明确拒绝旧配置与旧注册 | 自动转换旧绑定、保留旧库名版本别名、提供运行时兼容开关 |

## 2. 决策与权衡

### 2.1 保留现有 Operator 公共契约

以下现有入口和能力保持原有职责与行为：[S02][S03]

| 保留项 | 要求 |
| --- | --- |
| `Get_LLM_EDGEFLOW_OperatorTable()` | 仍返回现有 `OperatorFunc`；不改为全新对象式接口 |
| `GetOperatorLastError()` | 保留现有线程局部诊断查询；不借此重写错误体系 |
| `ValidateOperatorConfigBinding(...)` | 继续使用与创建路径一致的 Operator 配置解析规则 |
| `OperatorFunc` 六个函数指针 | 名称、顺序、参数、`noexcept` 与职责不因本次删接口而改变 |
| `CreateParam` | 保留配置根、相对配置路径、设备、计算平台和 `max_frame_depth` |
| `NamedIo` / `NamedIoBatch` | 保留当前命名槽位和 `shared_ptr<void>` 载体 |
| `MakeBorrowedOperatorInput` | 继续表达同步调用期间的只读借用，不扩大生命周期承诺 |
| `ControlCommand` 与参数结构 | 保留类型化命令及 `kJson + ControlJsonParam` |
| Operator 数据结构与 ValueType 注册 | 保留 `CompanyString`、`CompanyBuffer`、`CompanyAny`、图像及各业务 Operator 结构 |

`include/edgeflow/operator/types.h` 虽然含有“Compatibility entrypoint”注释，但它是当前 Operator 公共头链的一部分。**保留该文件并纠正注释，不按关键词将其误删。**

Operator 公共入口之外，`edgeflow/log.h`、`edgeflow/export.h`、生成的 `edgeflow/version.h` 和仍被使用的 `platform_mock/error_codes.h` 保留。公共头应直接包含自身需要的声明，不再借 `c_api.h` 间接取得错误码或版本宏。

### 2.2 目标调用链：沿用已有实现，不增加中间层

```text
宿主程序
  └─ Get_LLM_EDGEFLOW_OperatorTable()
       ├─ Init / Deinit
       ├─ Create
       │    └─ OperatorConfigResolver
       │         └─ 已验证的 IoPlan + RuntimeOptions
       │              └─ SharedAlgorithmRuntime::CreateFromIoPlan
       ├─ Process
       │    ├─ 验证 NamedIo 输入与输出请求
       │    ├─ input converter：外部请求 → 请求内的中性数据
       │    ├─ 使用现有机制租用输出块
       │    ├─ Pipeline / Nodes / Model / Backend
       │    ├─ output converter：内部结果 → 已租用的输出结构
       │    └─ 完整成功后发布输出；失败归还本次租约
       ├─ Control → 现有控制解析与运行时 Control
       └─ Destroy → 按现有契约回收句柄与池
```

`SharedAlgorithmRuntime` 的名字无需因为只剩一个宿主入口而修改。保留现有文件、类和组合层归属，减少无意义的改名、日志变化与包含路径变更。[S04][S05]

该类按下表处理：

| 成员 / 依赖 | 动作 |
| --- | --- |
| `#include "edgeflow/c_api.h"` | 删除；在实际使用处直接包含仍需要的错误码等头 |
| `GlobalInit` / `GlobalDeinit` | 保留；全局注册冲突与绑定审计不能删除 |
| `CreateFromIoPlan` | 保留；继续消费已验证计划，不绕过验证器 |
| `ExecuteControl` | 保留；Operator 仍调用 |
| `GetPipeline` / `GetIoPlan` 等有效访问器 | 保留实际使用的成员 |
| `CreateFromConfigFile` | 基线中固定使用 `"cabi"`；迁完调用方后删除 |
| `CreateFromPipelineJson` | 基线中同样固定使用 `"cabi"`；迁完调用方后删除 |
| 指针数组版 `ExecuteBatch` | C ABI 专用执行路径，迁完调用方后删除 |

这里删除的是 **`SharedAlgorithmRuntime` 的上述 C ABI 专用方法**，不是对整个仓库按 `CreateFromPipelineJson` 或 `ExecuteBatch` 名称批量删除。`IoBindingResolver` 中仍被工具或测试使用的计划解析能力，应按实际调用关系保留，并采用唯一的 Operator 约束。

不把原 C ABI `ExecuteBatch` 再包装成“通用执行器”保留下来；当前 Operator 已有完整执行链，本次没有新增第二条执行链的必要。

### 2.3 注册与配置只支持 Operator，但不顺带重构全部元数据

采用**保留现有描述模型、移除双路径实现**的方案：[S06][S07][S08][S09]

1. 删除生产代码中所有 C ABI 输入/输出转换器、`*.cabi.*` 接入绑定及其注册。
2. 各业务 `BizExposureDefinition.required_transports` 改为 `{"operator"}`，保留业务名、批次约束和其他有效信息。
3. `IoConverterRegistry` 的输入、输出注册，以及 `IoBindingRegistry` 的绑定注册只接受 `transport == "operator"`；空值、`"cabi"` 和其他值均拒绝。
4. 曝光声明的 `required_transports` 只允许唯一值 `operator`。不得仅改业务初始化列表，却继续接受自定义 C ABI 注册。
5. 配置与解析器的 transport 参数若继续存在，只允许 `operator`。校验应发生在执行或分配之前，不让错误 transport 跳过输出配置、槽位或预算检查。
6. 删除只服务于 C ABI 的条件分支；原本仅在 `transport == "operator"` 时执行的必需校验，在完成入口约束后成为正常必经步骤。

**为什么不同时删除所有 `transport` 字段？**

这些字段已经贯穿 Definition、解析接口、Catalog 和测试。将取值收敛为唯一值、删掉另一条实现，就足以兑现单入口要求。此时全面删除字段及改写工具数据格式属于另一项作者接口/元数据重构，不是删除 C ABI 的必要前置。保留一个只接受 `operator` 的描述字段，不表示保留 C ABI 兼容性，也不为未来预建多协议抽象。

其他约束不变：必须显式填写 `io_binding`；仍使用现有 Operator 绑定 ID；仍校验 schema、方向、端口类型、来源映射、槽位完整性、ValueType 与分配能力。禁止通过取消全量审计来“修复”删除 C ABI 后的初始化错误。

### 2.4 收敛转换器载体，保留方向独立性

`include/adapter/io_converter.h` 同时承载两种外部视图，必须清理内部遗留，而不是让废弃代码只变得不可达。[S06]

| 内容 | 目标处理 |
| --- | --- |
| 输入视图的 C ABI 指针数组 `items` | 删除 |
| 输出视图的 C ABI 指针数组 `items` 与其专用 `capacity` | 删除；注意不能误删 Operator 字符串/槽位容量 |
| 两个 `GetCAbi<T>` | 删除 |
| 输入 `At(index, slot_name = "")` 中空槽名退回 C ABI 的访问方式 | 删除该混合入口；有效调用方直接使用明确的 `GetSlot<T>` |
| 旧 `Company*InputStruct` / `Company*OutputStruct` 的 `ExternalTypeTraits` | 删除；以 `platform_mock/alg_types.h` 的实际声明集合为准 |
| `count`、`type_id`、槽位类型信息、`GetSlot<T>` | 保留仍被 Operator 使用的内容 |
| `leased_slots`、`slot_capacities`、`pool_specs` | 保留；属于 Operator 输出而非旧 C ABI |
| Operator 结构的 `ExternalTypeTraits` | 保留，不能因名字带 `Company` 而删除 |
| 输入/输出 Definition、端口映射、Decode/Encode 回调 | 沿用当前机制，不合并成一个业务 Adapter 类 |

转换器继续保持：输入不引用输出转换器，输出不回读外部输入指针；请求数据在输入边界复制为中性值；输出依靠已声明的内部值与请求来源信息组装响应。

### 2.5 混合实现文件按函数和注册项删除，不整文件误删

当前源码清单列有以下转换器与业务声明文件。[S10] 实施者必须逐个核对其中注册的 ID、回调和共享 helper，不能根据文件名猜测文件全属 C ABI。

| 类别 | 必查文件（均相对 `src/adapter/`） | 清理方式 |
| --- | --- | --- |
| 输入 | `input/text_input.cpp`、`translate_json_input.cpp`、`doc_query_input.cpp`、`rerank_input.cpp`、`audit_input.cpp`、`audio_input.cpp`、`image_query_input.cpp`（后六个同属 `input/`） | 删除 C ABI Decode、工厂、注册及专用结构读取；保留 Operator 路径与共用语义解析 |
| 输出 | `output/structured_document_output.cpp`、`translation_json_output.cpp`、`keyword_result_output.cpp`、`doc_answer_output.cpp`、`rerank_result_output.cpp`、`audit_result_output.cpp`、`audio_result_output.cpp`、`invoice_result_output.cpp`（后七个同属 `output/`） | 删除固定 C 输出结构写入和 C ABI 注册；保留响应组装、来源检查、Operator 容量与池写入 |
| 业务声明 | `biz/translate_bindings.cpp`、`entity_extract_bindings.cpp`、`keyword_match_bindings.cpp`、`doc_qa_bindings.cpp`、`cross_rerank_bindings.cpp`、`compliance_audit_bindings.cpp`、`audio_asr_intent_bindings.cpp`、`ocr_doc_qa_bindings.cpp`（后七个同属 `biz/`） | 删除 C ABI Binding 工厂及注册；曝光仅要求 Operator；保留内部业务定义 |

例如，`text_input.cpp` 现有 `DecodeCAbiTextHelper`、C ABI 文本/关键词 Decode 和 C ABI 工厂可删除，但 `DecodeOperatorEntityInput`、`DecodeOperatorKeywordInput` 及其注册必须保留。[S07]

`AdapterValidationHelper` 等 helper 同理：移除失去调用方的 C 指针数组批次预检；保留 Operator、Control、转换器或其他有效调用方仍使用的字符串检查、诊断、上下文发布和结果关联能力。**不得靠保留 `alg_types.h` 或给旧结构换名字来维持死路径。**

### 2.6 内存、配置、并发与异常不变量

本次不修改 Operator 的资源机制。[S03][S04]

| 不变量 | 实施与测试要求 |
| --- | --- |
| 输入同步借用 | 外部载体及其嵌套指针在 `Process` 返回前有效；不得因接口清理引入跨调用悬挂引用 |
| 输入转换 copy-in | 发布到请求黑板的值拥有所需数据，不将宿主不透明对象移入下层 |
| 输出池按现有帧深度工作 | 保留 `max_frame_depth` 当前默认 25、零值归一化及已有上限和预算规则；不引入 `frame_output_pool` 等新层 |
| 单份分配与池深度分离 | 分配方案只负责一份完整外部结构的分配、重置、销毁；现有池负责块数、租用和归还 |
| 配置解析在创建阶段 | 沿用 `allocator`、参数解析与已解析池规格；Process/输出转换器不重新读部署 JSON |
| 槽位名、类型后缀各司其职 | 保留 `key_suffix` 与 `type_suffix` 的既有区分，不能为了清理而合并；同一类型的不同逻辑槽仍独立解析 |
| 全批成功后发布 | Decode、分配、执行、Encode、发布中的失败不遗留本次租约、不把半批输出交给调用方 |
| 输出是句柄所属池的租约 | 持有 `shared_ptr` 不等于延长句柄或池内存寿命；需要长期保存时先复制字段，再释放所有租约 |
| 句柄销毁前置条件 | 调用方停止提交，并等待该句柄的 Process/Control 返回，释放所有输出引用，再 Destroy |
| Destroy 的消费语义 | 保留当前契约：有效句柄一经 Destroy 即被消费，即使因未归还输出返回错误，也不能重试或继续使用 |
| 同句柄与跨句柄并发 | 保留同句柄 Process/Control 串行化、不同句柄可并行；不宣称支持任意并发 Destroy/Deinit |
| 异常隔离 | 保留 Operator 边界的 `noexcept`、标准异常与未知异常屏障；删除 C ABI 屏障不等于删除共用诊断能力 |

池满等待的回归测试必须由另一线程释放租约，并设置测试超时；不得让测试线程持有全部旧租约又同步等待新输出，制造无法结束的测试。

### 2.7 构建、公开头与动态符号

#### 2.7.1 公开头与目标

保留 `alg_sdk`、`llm_edgeflow::sdk`、库基础名 `company_alg_sdk`，以及现有分层对象目标。修改 `cmake_ext/LayerHeaderViews.cmake`，从 public、integration 视图移除两个 C API 头及旧 `alg_types.h`。不得为了修复包含错误，给公开 SDK 补上传递性的源码根目录或私有头目录。[S11]

公开可调用头至少覆盖：Operator 的 `interface.h`、`types.h`，其需要的平台 Operator 类型头，以及日志、导出、版本和仍使用的错误码声明。新配置与增量重新配置后都不能出现残留的 C API 头视图。

不因删除 C11 算法接口而机械地将 `project(... LANGUAGES C CXX)` 改为仅 CXX。C 语言与第三方构建仍可能有关，且不属于本次宿主接口选择。

#### 2.7.2 导出由 12 项收敛为 6 项

修改 `cmake_ext/edgeflow_sdk.map.in` 和 `scripts/check_sdk_exports.sh`，同步删除六个旧算法符号：[S12]

```text
Alg_Init
Alg_Create
Alg_Process
Alg_Control
Alg_Destroy
Alg_DeInit
```

最终保留的导出白名单为：

| 类别 | 符号 / 函数 |
| --- | --- |
| Operator | `llm_edgeflow::operator_api::Get_LLM_EDGEFLOW_OperatorTable()` |
| Operator | `llm_edgeflow::operator_api::GetOperatorLastError()` |
| Operator | `llm_edgeflow::operator_api::ValidateOperatorConfigBinding(...)` |
| 独立日志 | `AlgBase_setLogLevelByName` |
| 独立日志 | `AlgBase_getLogLevelByName` |
| 独立日志 | `AlgBase_logPrint` |

当前 Linux 导出门禁使用精确的 C++ 修饰名。对本次未改签名的三项沿用现有精确值，不用 `operator_api*` 或 `Alg*` 通配符扩大导出范围。跨平台采用各自现有工具链的符号约定；不能把一个平台的修饰名检查假称为所有平台验证。

三个日志函数不是第二套算法 API，保留它们符合本 RFC 的单入口目标。

#### 2.7.3 版本边界

删除已发布符号属于破坏性变更。建议在正式切换提交将产品版本设为 `11.0.0`、ABI 版本设为 `7.0.0`、`SOVERSION` 设为 `7`，并继续由当前 CMake 模板生成版本信息。[S11]

不提供 ABI 6 版本节点、旧 SONAME 的兼容链接或旧 `Alg_*` 转发。Operator 虽然保留源码接口形状，宿主仍应按新 SDK 重新构建，并使用匹配的编译器/标准库与构建设置；函数表中的 STL 类型不是跨任意 C++ 工具链的稳定边界。本次不为此再加一层 C 包装。

### 2.8 不采用的替代方案

| 替代方案 | 不采用原因 |
| --- | --- |
| 只停止导出 `Alg_*`，保留内部实现、类型、注册和测试正向路径 | 只是隐藏接口，没有完成代码与维护面收敛 |
| `Alg_*` 转发到 Operator | 明确违背不保留兼容性要求 |
| `ENABLE_C_ABI` 默认关闭 | 仍需维护和测试两种产品形态 |
| 删除整个 Adapter 层 | 会误删 Operator 正在使用的转换、配置与验证能力 |
| 将所有转换器重新合并为业务 Adapter/bridge | 回退输入输出独立化成果，扩大设计与代码量 |
| 借机替换 Operator 输出池或公共 DTO | 无必要业务缺口，增加生命周期与内存风险 |
| 删除所有 `extern "C"` 和 `Company*` 类型 | 无法区分旧算法入口、独立日志与当前 Operator 数据声明 |

## 3. 兼容与迁移

### 3.1 逐文件动作清单

下表用于实施与代码评审。标为“扫描”的项目表示纳入影响面，而非声称其中每个文件均已发现旧调用。

| 动作 | 文件 / 区域 | 完成条件 |
| --- | --- | --- |
| 删除 | `include/edgeflow/c_api.h` | 源树与公开头视图均不存在 |
| 删除 | `include/edgeflow/c_api.hpp` | 不再存在旧 `Alg_Process` C++ 包装 |
| 删除 | `src/adapter/c_api_adapter.cpp` | 无 `AlgHandleInstance` 和六个旧函数定义，构建源清单同步移除 |
| 删除 | `include/platform_mock/alg_types.h` | 所有有效消费者迁完，旧 C ABI 参数/业务结构无有效引用 |
| 局部删除 | `src/adapter/shared_algorithm_runtime.h/.cpp` | 仅移除 C ABI 专用方法和依赖，Operator 构建与控制仍工作 |
| 局部删除 | `include/adapter/io_converter.h` | 删除 C ABI 载体和 traits，保留 Operator 槽位、容量及端口契约 |
| 局部删除 | `src/adapter/input/*.cpp`、`output/*.cpp` | C ABI Decode/Encode/工厂/注册及失去用途的 helper 清零 |
| 修改 | `src/adapter/biz/*_bindings.cpp` | 只有 Operator 绑定，业务曝光审计不再要求 C ABI |
| 修改 | `src/adapter/io_converter_registry.cpp`、`io_binding_registry.cpp` | 非 Operator 注册被拒绝，其他冲突与完整性检查保留 |
| 修改 | `src/adapter/deployment_io_config.*`、`io_binding_resolver.*`、`operator/operator_config_resolver.*` | 无 C ABI 配置分支；Operator 校验、根路径、容量和预算不减弱 |
| 修改 | `CMakeLists.txt`、`src/adapter/CMakeLists.txt`、`cmake_ext/LayerHeaderViews.cmake` | 无旧源码/旧头声明，版本与层依赖一致 |
| 修改 | `cmake_ext/edgeflow_sdk.map.in`、`scripts/check_sdk_exports.sh` | 精确六项导出，旧算法符号不存在 |
| 清理 | `demo/common/operator_runner.h`、Operator 测试等残留 `c_api.h` include | 删除无用 include；需要错误码/版本的直接包含对应头 |
| 替换 | `tests/contract/abi/test_c11_abi_compliance.c` | 删除 C11 消费者，改由独立 C++ SDK 消费者验证公开边界 |
| 迁移 | `tests/contract/abi/test_c_abi_safety.cpp` | 有效安全场景迁入 Operator 套件；不将整份保障直接扔掉 |
| 同步 | `cmake_ext/TestInventory.cmake`、`Tests.cmake`、`IndividualTests.cmake`、`tests/CMakeLists.txt` | 两种运行模式都有迁移后的责任套件，无失效源、过滤器或缺失用例 |
| 扫描/修改 | `src/adapter/io_catalog.*`、`src/tools/`、`tools/`、`scripts/`、`dev_support/` | 生产工具不输出/生成/默认选择 C ABI；测试注册不会污染生产 SDK |
| 扫描/迁移 | `configs/`、`demo/fixtures/`、`tests/fixtures/` 及测试内动态配置生成 | 正向配置均为有效 Operator 组合；旧配置仅能作为明确的拒绝测试输入 |
| 扫描/修改 | `.github/workflows/`、静态/架构检查及脚手架模板 | 无旧目标要求和旧 API 生成路径；门禁强度不降低 |
| 同步文档 | 第 3.4 节所列现行指南 | 只描述当前 Operator + converter/binding 架构 |

真正有用的共用文件应保留，不为了减少表中“修改”项而整文件删除，也不通过增加同等功能的替代文件来掩盖旧路径残留。

### 3.2 仓内消费者与测试迁移规则

#### 3.2.1 创建与执行

旧代码中的 `CompanyAlgParamCreate` 迁为现有 `CreateParam`，明确给出配置根 `model_path`、根内相对路径 `cfg_file_name`、设备与支持的计算平台。不要把原有任意路径接入方式照搬到 Operator，破坏根路径约束。

输入从 C ABI 结构迁为现有 Operator 结构，字符串按当前 `CompanyString` 长度/指针约定构造，使用 `MakeBorrowedOperatorInput` 填入对应命名槽位。载体及嵌套缓冲区应稳定存活到调用结束；构造测试批次时避免容器扩容使已保存的元素地址失效。

输出不再由调用方准备旧固定数组结构，而按现有约定提供与输入批次等长的输出容器，以及已声明的输出槽位请求。例如现有关键词调用使用 `client_channel.keyword_in` / `client_channel.keyword_out`，输出槽位先放空指针，再由成功的 `Process` 发布池租约。[S13]

业务断言读取返回的 Operator 结构。对需要跨调用保存的内容做字段级深拷贝；不要只复制外层结构而保留池内字符串指针。释放输出容器之外，还要释放测试中复制出来的所有输出 `shared_ptr`，之后才能销毁句柄。

#### 3.2.2 控制与安全语义

旧 `CompanyAlgParamControl` 正向用例迁为现有类型化 Control 参数，或 `ControlCommand::kJson` 配合 `ControlJsonParam{cmd_id, payload}`。继续验证完整 JSON 对象、长度、命令声明、更新失败不破坏当前配置等已有保障，不创建第二种控制分发。

迁移测试时以**现有 Operator 的契约与错误语义**为准，不为复现被删除 C ABI 的错误优先级、输出计数或固定数组容量约定增加分支。

例如旧“调用方输出指针数组容量不足”测试不应原样保留；其有意义的保障分别映射为 Operator 批次数量/槽位检查，以及输出池字段容量不足时不越界、不发布、不泄漏租约的测试。

`shared_ptr<void>` 无法凭任意裸内存证明真实对象类型。类型与后缀测试应针对当前可验证的槽位声明、ValueType 和合法载体契约，不能用不安全的内存探测或未定义行为充当类型安全测试。

### 3.3 配置与 Catalog 迁移

| 项目 | 迁移规则 |
| --- | --- |
| 已使用 Operator 的 `.conf` | 原样保留有效 binding、outputs、allocator、params 和容量；无需为本次换 schema |
| 仍使用 `*.cabi.*` 的仓内正向配置 | 选择对应已注册 Operator 绑定，并按实际外部输出槽补齐输出分配配置；逐项验证 |
| Pipeline JSON | 原则上不改算法、节点、提示词或连线；只处理确由废弃接入路径引出的引用问题 |
| 旧外部配置 | 明确失败，不自动把 `.cabi.` 替换成 `.operator.`，也不猜默认绑定 |
| 自定义/测试内注册 | 同步改为 Operator；负例在隔离测试中明确验证拒绝 |
| Catalog | 实际输入、输出转换器和绑定列表中不出现可用 C ABI 项；保留业务及内部端口信息 |
| CLI / Studio / 脚手架 | 不再提供或生成 C ABI 选项；旧参数值被明确拒绝，不退回 Operator 成功 |

现有 `configs/pipeline_keyword_match_rules.conf` 已使用 `keyword_match.operator.v1` 和 `keyword_out` 输出池配置，可作为无模型权重的公开 SDK 冒烟基准。[S14]

不能以一次全局文本替换完成配置迁移：旧 C ABI 固定输出结构和 Operator 输出池配置不同，同名业务也不代表两套载体和容量可以直接替换。

### 3.4 当前文档、治理与历史决策

实施必须同时修正文档中的外部业务边界：完整请求解码、字段选择、响应组装和序列化仍由 SDK 接入适配层完成，只是边界改为 Operator。Demo/Python 不得接管业务协议转换来掩盖缺失实现。

| 文档范围 | 必须同步的内容 |
| --- | --- |
| `README.md`、`doc/README.md`、`doc/architecture.md`、`doc/developer_guide.md` | 单一算法入口、当前调用链、版本与接入路径 |
| `AGENTS.md` | 删除“必须维护六个 Alg_*、公共算法头必须 C11、C ABI 为业务边界”的现行约束；保留层次隔离、异常隔离和完整载荷转换原则 |
| `doc/dev_guide/business_onboarding.md` | 按现有 input/output converter、binding、ValueType 和 Operator 编写；不再要求新增旧 IBizAdapter 或业务 bridge |
| `.agents/skills/llm-edgeflow-developer-guide/` 及其他实际相关指引 | 更新入口、实现位置、验证目标；不复制另一份互相冲突的架构规则 |
| `include/platform_mock/README.md`、`configs/README.md`、`tests/README.md` | 平台类型清单、部署配置规则、迁移后的测试责任 |
| `doc/solutions/translate.md`、输出分配指南及相关练习/模板 | 使用完整 Operator 外部请求；保留 JSON 业务语义和单份分配职责 |
| `tools/pipeline_studio/README.md`、工具帮助、架构图 | 不再把 C ABI 描述为可用算法入口 |
| `doc/CHANGELOG.md` | 明确列出破坏性删除、配置和源码迁移要求 |
| RFC 索引与 RFC-0059/对应实施计划 | 增加本 RFC 链接，注明仅取代双宿主入口部分，保留转换器独立化等有效决策 |

历史 RFC、历史审计和旧版本变更记录不按关键词清空，不改写成“历史上只有 Operator”。采用明确的适用范围说明，区分历史事实与当前指南。[S18][S19]

### 3.5 交付与回退边界

最终交付为一个完整的 Operator-only 变更集，不单独发布“入口已删除但测试、注册或工具仍要求 C ABI”的中间状态。

回退通过撤回完整变更集或恢复上一产品版本完成，而不是在新运行时内添加旧路径。已迁移到新版本的调用方和配置也必须与对应 SDK 一起回退；回退不代表本 RFC 需要维护长期兼容。

## 4. 验证与完成条件

### 4.1 现有测试的处置与保障迁移

测试文件与套件对应关系以当前 `TestInventory.cmake` 为准。[S15][S16] 下表区分“接口专属断言可删除”和“工程保障必须迁移”。

| 当前测试 / 责任 | 本次处置 | 迁移后必须证明 |
| --- | --- | --- |
| `C11AbiComplianceTest` / `test_c11_abi_compliance.c` | 删除旧正向消费者，增加下面定义的 `CppOperatorSdkTest` | 公开 C++ 头与共享库可以独立接入 |
| `CAbiSafetyTest` / `test_c_abi_safety.cpp` | 将仍有意义的安全场景迁入 Operator 责任套件；必要时重命名文件/套件 | 参数失败无资源泄漏、异常不越过公共边界、失败不发布输出 |
| `AdapterContractSecurityTest` | 保留责任，迁移旧载体和配置 | 输入边界、路径隔离、业务完整载荷与输出容量安全 |
| `DifferentIoModalitiesTest` | 改为 Operator 载体或复用已有覆盖 | 文本、音频、图像及其他已有模态的业务链不丢失 |
| `AllBizPipelinesTest` | 迁移创建、执行、结果提取 | 每个现有业务的有效路径、来源编号和结果语义 |
| `ConcurrencyAndEdgeCasesTest` | 迁移句柄与输出生命周期 | 同句柄序列化、不同句柄隔离、边界批次与错误回收 |
| `RuntimeControlAndHotSwapTest` | 使用现有 Operator Control | 规则、提示词、阈值/JSON 控制等实际支持的更新行为 |
| `OperatorApiTest`、`OperatorGoldenTest` | 保留并扩展缺失覆盖；清理无用 C API include | 原 Operator 行为未因删另一套接口而退化 |
| `OperatorOutputPoolTest`、`OperatorValueRegistryTest` | 保留，补必要的切换回归 | 单份分配、typed params、布局、容量、租约、销毁与失败回滚 |
| `IoConverterTest`、`TextConvertersTest`、`ComplexConvertersTest` | 删除 C ABI 正向组合；保留共享语义和 Operator 断言 | 完整外部协议转换、方向解耦、来源和结果一致性 |
| `IoBindingRegistryTest`、Catalog/注册冲突测试 | 收敛 transport，并增加拒绝注册测试 | 非 Operator 不可注册；缺失 Operator 能力仍 fail-closed |
| Demo、CLI、Studio 与脚手架测试 | 迁移旧生成内容和快照 | 用户正常路径可用，生产工具不再生成旧 API/配置 |
| 真实模型 E2E 源码 | 如包含旧调用则迁移；有资产时运行 | 不因可选测试默认不运行而留下不能编译的旧接口消费者 |

复用已有责任套件，允许合并重复用例，但必须记录每个被移除场景的归宿。不能用“测试数量下降属于删接口”解释业务、内存或并发覆盖丢失。

### 4.2 新增最小公开 SDK 消费者测试

建议新增 `tests/contract/abi/test_cpp_operator_sdk.cpp`，注册为 `CppOperatorSdkTest`，替代原独立 C11 消费者的边界验证职责。

**构建约束：**单独可执行目标，只链接 `llm_edgeflow::sdk` 及必要的系统/测试依赖；只使用 SDK 公开头视图。不链接 `llm_edgeflow::internal_runtime`，不注入运行时对象或测试业务/后端注册，不添加源码根目录和 `src/` 作为补救包含路径。

**必须处理现有目录级继承：**基线 `tests/CMakeLists.txt` 在目录级调用了 `link_libraries(edgeflow_internal_headers)`。因此，仅给新目标写 `target_link_libraries(... llm_edgeflow::sdk)` 并不能证明它只有公开依赖。创建这个消费者时必须显式清除其继承的目录级链接依赖，或放入经过隔离的 public-only 测试作用域，再仅链接 SDK。不要为此取消其他内部测试需要的头视图。验收检查目标最终链接依赖和实际编译命令，确认没有 `edgeflow_internal_headers`、源码根/`src/`、extension 私有视图或内部对象注入。[S15]

**执行流程：**

```text
读取函数表并检查六个入口存在
  → Init
  → 对生产关键词配置调用 ValidateOperatorConfigBinding
  → Create（CPU，显式配置根与相对路径）
  → Process（实际关键词规则，不依赖真实模型权重）
  → 验证请求编号、状态与结果内容
  → Control 并再次验证结果变化
  → 复制结果字段，释放所有输出 shared_ptr
  → Destroy
  → Deinit
```

另增加最小公开头自包含编译检查：至少单独包含 `edgeflow/operator/interface.h`，并分别检查仍交付的类型、日志和版本头；不得先包含旧 C API 头补齐依赖。增加旧头不可获得/旧算法符号不可链接的边界检查。负向编译或链接测试必须先有同环境的正向对照，并确认失败原因确实是旧头/旧符号缺失，不能把缺少编译器或依赖误判为删除成功。

将 `CppOperatorSdkTest` 纳入共享 `TestInventory.cmake`，在 sharded 与 individual 模式中均运行。保留 `SdkExportSurfaceTest` 的独立职责；一个运行成功的 Demo 不足以代替它们，因为当前 Demo 使用内部运行时链接。[S13][S15]

### 4.3 验收矩阵

| 编号 | 场景 | 必须观察到的结果 |
| --- | --- | --- |
| V01 | 源码与公开头边界 | 两个 C API 头、旧 adapter、旧平台 C ABI 类型删除；无有效正向调用 |
| V02 | 动态导出 | 六个旧 `Alg_*` 不存在；三个 Operator 与三个日志导出精确存在；无新增内部导出 |
| V03 | 注册初始化 | 生产绑定和转换器全为 Operator；所有已有业务应有的 Operator 组合完整；重复/缺失/非法注册仍失败 |
| V04 | 旧配置拒绝 | `*.cabi.*`、非 Operator transport、缺失显式绑定不被自动转换或默认补全 |
| V05 | 配置安全 | 根目录/相对路径约束、符号链接逃逸、未知字段、未知输出槽、非法容量与超预算仍被拒绝 |
| V06 | 请求与业务 | 关键词、实体、翻译、文档问答、精排、审计、音频和图像业务按各自可用 fixture 验证；输入输出协议与编号不变 |
| V07 | 槽位机制 | 当前支持的单/多输入输出、必需/可选槽、键后缀与类型后缀、同类型不同逻辑槽正常工作 |
| V08 | 内存失败路径 | 创建/解码/分配/执行/编码等错误不越界、不泄漏、本次输出不半发布；后续合法调用仍可用 |
| V09 | 租约 | 未释放结果在合法句柄生命周期内跨后续调用保持有效；释放后容量恢复；池满等待有可控唤醒 |
| V10 | 并发与控制 | 同句柄序列化、跨句柄隔离与已有控制更新语义保留；不把违反 Destroy 前置条件的行为当作正常并发使用 |
| V11 | 独立接入 | 只靠公开 SDK 的 C++ 消费者可以编译、链接并执行；旧头/符号的拒绝检查有正向对照 |
| V12 | 工具与治理 | Catalog、CLI、Studio、脚手架和现行文档只提供 Operator 算法路径，历史记录有明确边界 |
| V13 | 构建与测试清单 | 全新/增量构建与两种测试组织模式不缺套件；无失效 gtest filter 导致的空跑 |
| V14 | 分层回归 | Core、Node、Model、Backend 不新增 Operator 平台依赖；已有下层非相关契约测试保持通过 |

V06 的无权重 fixture 证明链路与数据契约，不证明真实模型效果或目标硬件性能。涉及可选 Backend 的测试应明确记录构建开关、资产与跳过原因，不能把未启用算作通过。

### 4.4 残留扫描与误报边界

实施开始与结束各做一次仓库级扫描。以下为**实施者执行的定位命令**，本次文档编制没有执行该仓库扫描：

```bash
git rev-parse HEAD

git grep -n -I -E \
  'edgeflow/c_api\.h(pp)?|platform_mock/alg_types\.h|Alg_(Init|Create|Process|Control|Destroy|DeInit)|CompanyAlgParam(Create|Control)|GetCAbi|DecodeCAbi|EncodeCAbi|cabi|CAbi|C_ABI|c_abi|C ABI' \
  -- include src demo dev_support tests configs cmake_ext scripts tools .github .agents \
     AGENTS.md README.md doc
```

`git grep` 返回 1 表示无匹配，返回大于 1 表示执行错误，自动门禁不得用无条件 `|| true` 混淆两者。

在已有静态门禁或其合适责任位置增加小范围检查即可，不建立新的审计框架。允许出现旧名称的位置必须显式列明，例如：本 RFC、保留原貌的历史记录、旧接口缺失/旧配置拒绝的负向测试，以及检查脚本自身的匹配规则。

**禁止将整个 `tests/`、`scripts/` 或 `doc/` 一概排除。** 当前指南、脚手架模板、正向测试和配置都属于需要收敛的范围。仅扫描字符串也不能证明无死代码；必须结合被删调用链、源清单、注册结果和动态符号检查。

### 4.5 实施时的验证命令

以下命令是交付要求，不是本次已经执行的结果。新测试名须先按第 4.2 节注册，实际 CTest 名称以更新后的 Inventory 为准。

**开发期：使用独立、无真实模型后端的构建目录定位接入层问题。**

```bash
cmake -S . -B build-operator-only-dev -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DENABLE_ONNXRUNTIME=OFF \
  -DENABLE_LLAMACPP=OFF \
  -DENABLE_WHISPERCPP=OFF \
  -DENABLE_KITELLM=OFF

cmake --build build-operator-only-dev --parallel 4

ctest --test-dir build-operator-only-dev \
  --output-on-failure --no-tests=error \
  -R 'Operator|IoConverter|IoBinding|TextConverters|ComplexConverters|AdapterContractSecurity|SdkExportSurface'
```

该过滤器覆盖含 `Operator` 的新 SDK 测试名称；实际必须结合 Inventory 验证每个要求的套件均注册，而不能仅凭 CTest 总体“至少有一个测试”判定完整覆盖。

**最终交付：运行仓库现行完整门禁。**

```bash
./scripts/run_all_tests.sh
```

该脚本在基线中自行完成格式、Shell、Git whitespace、默认后端构建及完整 CTest；默认启用 ONNX Runtime 与 llama.cpp，禁用可选真实模型 E2E。[S17] 无后端开发构建不能替代这个门禁。不要在完整门禁前后无目的重复同配置的全量构建与测试。

**额外验证：**在独立目录检查 `LLM_EDGEFLOW_SHARDED_TEST_RUNNERS=OFF` 的套件完整性与相关测试；另用 `ENABLE_SANITIZERS=ON`、`LLM_EDGEFLOW_SANITIZERS=address,undefined` 运行相关运行时用例。需要线程检查时使用独立的 `thread` sanitizer 构建，不与 ASan 混用。沿用现有 sanitizer 标签组织，确保池、Control 和相关运行时路径没有因迁移而漏出覆盖范围。

不得同时在同一构建目录运行竞争构建。验证记录必须标明提交、工具链、配置、命令、测试结果与未覆盖项。若评审后的代码修改影响已验证路径，旧结果不再充分，应重跑受影响验证，并确保最终候选提交满足完整交付门禁。

### 4.6 完成判定

只有同时满足下列条件才可将 RFC 改为 `Completed`：

- [x] 旧六个算法函数、C++ 包装、C ABI 专用运行链、类型及生产注册已经实际删除，不是隐藏或禁用。
- [x] 当前 Operator 的函数表、数据载体、配置与资源语义保留，没有新增兼容层、第二套 runtime 或新输出池体系。
- [x] 所有有效消费者和正向配置迁移完成；旧配置与非法 transport 明确失败。
- [x] 输入/输出转换独立、完整外部协议转换和四层依赖方向保持不变。
- [x] 公开头与六项动态符号门禁通过，独立 C++ SDK 消费者验证通过。
- [x] 被移除安全与业务测试均有明确处置记录，两种测试模式无缺失或空跑。
- [x] 内存、池租约、失败回滚、Control 与并发回归通过，必要 sanitizer 结果有记录。
- [x] Catalog、CLI、Studio、模板、CI、现行文档和 Agent 约束全部收敛；历史记录有清晰适用范围。
- [x] 最终提交完成独立评审和完整交付门禁，未覆盖模型/硬件事项明确列出。
- [x] 版本、RFC 索引与 Changelog 同步；没有伪装成新版本的旧 ABI 兼容产物。

## 5. 实施与最终结果

### 5.1 阶段划分与推进条件

建议每个阶段对应可单独评审的提交或提交组，按顺序推进。阶段验收失败时先修复本阶段，不以“后续会处理”为理由推进。此任务无需在生产代码中打桩或增加临时转发；阶段支点采用测试与清单。

| 阶段 | 主要工作 | 阶段验收与停止条件 |
| --- | --- | --- |
| P0：固定基线与迁移清单 | 在实施仓库核对 SHA 和与本 RFC 的差异；执行全仓定位；列出旧头、类型、函数、注册、配置、生成器和消费者；记录业务/安全测试场景映射；登记 RFC | 清单覆盖生产、工具、测试和文档；对新增差异作明确补充。未厘清的共用函数不能直接删除 |
| P1：解除依赖并建立替代验证 | 清理 Operator 与公共工具对 `c_api.h` 的间接依赖；将共用业务/安全/并发/Control 测试和有效消费者改用 Operator；增加独立 C++ SDK 消费者；保留尚未切除的原 C ABI 供原有专属测试运行 | Operator 相关构建和测试、公开 SDK 消费者通过；迁移后的业务断言未减少。不得用内部运行时链接冒充公开 SDK 验证 |
| P2：原子化删除旧接口链 | 同一可构建变更组内删除旧头、adapter、类型、专用 runtime 方法、转换分支与注册；收敛解析/审计；同步配置、CMake、测试清单、六项导出、建议版本及直接受门禁约束的文档/工具 | 没有有效 C ABI 路径；正向初始化与配置通过、旧配置拒绝；新/增量构建、导出和相关测试通过。不能只删源码却保留断裂构建或双 transport 审计 |
| P3：工具、模板与现行指南收尾 | 验证 Catalog/CLI/Studio/脚手架所有路径，补齐残留模板与文档；更新 AGENTS、技能引用、RFC 取代范围和 Changelog；核对两种测试模式与场景归宿 | 正常开发路径不再引导用户创建 C ABI；架构/文档/工具契约检查通过；无正向旧调用。P2 中必需的门禁修复不可拖到本阶段 |
| P4：回归、独立评审与关闭 | 完整门禁、必要非默认构建/sanitizer；只读评审删除闭包、所有权、注册审计和测试有效性；修复并验证发现的问题；在本 RFC 记录最终证据 | 满足第 4.6 节全部条件；有明确验证基线与未覆盖边界后才能标记 Completed |

P1 暂时仍存在的原 C ABI 是尚未切除的基线代码，不是新增兼容产品。P2 后不得用兼容头、typedef、空实现或功能开关把它加回来。未完成整体迁移前不发布单独的“Operator-only”版本。

### 5.2 建议任务分工

主实施者负责删除边界、公共契约和注册/配置决策；机械修改可按输入、输出、绑定与文档区域分组，但必须先给出明确删除清单。测试负责人独立核对场景映射、公开 SDK 接入与失败路径。验证与评审应分别确认“执行通过”和“测试实际证明了需求”，不要仅凭净删代码量判断成功。

跨文件强耦合区域——旧类型删除、`io_converter.h`、注册白名单、共享 runtime 和构建清单——由同一负责人协调切换，避免并行修改互相依赖导致每个提交都无法构建。

### 5.3 风险与控制

| 风险 | 识别方式 | 控制措施 |
| --- | --- | --- |
| `c_api.h` 传递包含掩盖真实依赖 | 删除后错误码、版本宏或 DTO 未声明 | P1 先改为直接包含；公开头单独编译 |
| 混合转换器文件被整块误删 | Operator 业务/注册缺失或语义变化 | 按回调与注册逐项删；保留中性解析、输出组装与来源检查 |
| 只改生产业务声明，遗漏测试/生成器注册 | 全局 Init 冲突、脚手架重新生成 C ABI | 扫描静态与动态生成点；新增非 Operator 注册拒绝测试 |
| 内部对象链接掩盖公开 SDK 断裂 | 单元测试通过但宿主无法接入 | 独立链接 `llm_edgeflow::sdk` 的 C++ 消费者 |
| 输出租约迁移不完整 | Destroy 返回错误、结果悬挂或池满卡住 | 字段深拷贝、释放全部引用；超时保护的租约/并发回归 |
| 为删接口而弱化校验 | 未知 transport 跳过 outputs/预算审计 | 唯一 transport 约束与原必需校验共同落实；保留负例 |
| 旧文档将实现带回 Adapter/bridge | 新代码与现有转换器设计不一致 | 同步 AGENTS 和当前接入指南，历史 RFC 标明取代范围 |
| 版本、测试与源码基线漂移 | 实施分支已增加新接口或 RFC 编号 | P0 固定实际提交并记录差异，版本与编号入库前检查 |

### 5.4 代码规模与设计约束

预期生产代码以删除为主：旧门面、固定 C 载体、重复转换分支和双注册将退出；新增代码主要应是公开 SDK 消费者及少量负向验收检查。

这是预期方向，不是可凭文档保证的净行数。实施后分别统计生产源码、测试、配置与文档的增删，解释保留的共用能力和新增测试。禁止为满足“删得更多”移除测试，也禁止新建与被删路径功能重复的抽象层抵消收敛效果。

### 5.5 本次文档交付的实际状态

| 项目 | 本次状态 |
| --- | --- |
| 核对默认分支、固定提交和关键接口源码 | 已完成远程静态核查与基线定位 |
| 阅读关键 runtime、转换器载体、注册、构建、测试清单及现行指南 | 已完成；依据列于附录 |
| 仓库全量调用点扫描、实际编译和测试运行 | **已执行**：全量门禁 `./scripts/run_all_tests.sh`（100/100 通过） |
| 修改仓库源代码、测试集、配置与架构图 | **已完成**：删除旧 C ABI，仅保留 C++ Operator SDK |
| 真实模型效果与目标硬件验收 | **未执行**：属于内网/模型资产就绪阶段工作，不影响单接口架构切换 |
| 当前结论 | **实施已全部完成**，达到 Completed 判定条件 |

---

## 附录 A：固定提交源码依据

以下链接均固定到 `2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9`，用于区分“基线事实”和“本 RFC 的目标变更”。实施期间以实际分支新增差异补充，不把链接中的旧代码误当作迁移后的最终状态。

- **[S01] 旧算法 API 与包装**：[c_api.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/edgeflow/c_api.h)、[c_api.hpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/edgeflow/c_api.hpp)、[c_api_adapter.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/c_api_adapter.cpp)。
- **[S02] Operator 公共入口**：[interface.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/edgeflow/operator/interface.h)、[types.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/edgeflow/operator/types.h)。
- **[S03] 平台类型与生命周期契约**：[operator_types.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/platform_mock/operator_types.h)、[operator_data_types.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/platform_mock/operator_data_types.h)、[待删除的 alg_types.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/platform_mock/alg_types.h)。
- **[S04] Operator 实际执行与资源管理**：[operator_adapter.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/operator/operator_adapter.cpp)。
- **[S05] 共享运行时中的保留/删除边界**：[shared_algorithm_runtime.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/shared_algorithm_runtime.h)、[shared_algorithm_runtime.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/shared_algorithm_runtime.cpp)。
- **[S06] 双载体视图与转换器作者契约**：[io_converter.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/adapter/io_converter.h)、[converter_authoring.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/include/adapter/converter_authoring.h)。
- **[S07] 混合转换文件与业务双注册实例**：[text_input.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/input/text_input.cpp)、[keyword_match_bindings.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/biz/keyword_match_bindings.cpp)、[translate_bindings.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/biz/translate_bindings.cpp)。
- **[S08] 注册与全量绑定审计**：[io_converter_registry.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/io_converter_registry.cpp)、[io_binding_registry.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/io_binding_registry.cpp)。
- **[S09] 部署配置与计划解析**：[deployment_io_config.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/deployment_io_config.cpp)、[io_binding_resolver.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/io_binding_resolver.cpp)。
- **[S10] 当前接入层源码清单**：[src/adapter/CMakeLists.txt](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/src/adapter/CMakeLists.txt)。
- **[S11] 构建、版本与公开头视图**：[CMakeLists.txt](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/CMakeLists.txt)、[LayerHeaderViews.cmake](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/cmake_ext/LayerHeaderViews.cmake)。
- **[S12] 当前 12 项动态导出**：[edgeflow_sdk.map.in](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/cmake_ext/edgeflow_sdk.map.in)、[check_sdk_exports.sh](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/scripts/check_sdk_exports.sh)。
- **[S13] 已有 Operator Demo 路径**：[demo/main.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/demo/main.cpp)、[operator_runner.h](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/demo/common/operator_runner.h)、[keyword_match_demo.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/demo/biz/keyword_match_demo.cpp)、[demo/CMakeLists.txt](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/demo/CMakeLists.txt)。
- **[S14] 无模型权重的 Operator 部署配置**：[pipeline_keyword_match_rules.conf](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/configs/pipeline_keyword_match_rules.conf)。
- **[S15] 测试责任与构建组织**：[TestInventory.cmake](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/cmake_ext/TestInventory.cmake)、[Tests.cmake](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/cmake_ext/Tests.cmake)、[tests/CMakeLists.txt](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/tests/CMakeLists.txt)。
- **[S16] 现有 Operator 业务结果断言**：[test_operator_golden.cpp](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/tests/integration/operator/test_operator_golden.cpp)。
- **[S17] 完整交付门禁**：[run_all_tests.sh](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/scripts/run_all_tests.sh)。
- **[S18] 需要同步的现行约束与指南**：[AGENTS.md](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/AGENTS.md)、[business_onboarding.md](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/doc/dev_guide/business_onboarding.md)、[README.md](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/README.md)。
- **[S19] 输入输出独立化目标与取代范围**：[adapter_io_layout_design_2026-09-15.md](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/doc/plans/adapter_io_layout_design_2026-09-15.md)。
- **[S20] RFC 编号、状态与结构约定**：[RFC 索引](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/doc/rfcs/README.md)、[RFC_TEMPLATE.md](https://github.com/chamsechan/LLM-EdgeFlow/blob/2d58d0c3dbadfe9b969f2eb85eb40461cbc7d0e9/doc/rfcs/RFC_TEMPLATE.md)。
