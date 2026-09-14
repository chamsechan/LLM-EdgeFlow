# RFC 0053：业务 Adapter 函数式作者接口与载体机制收敛

- **RFC 编号**：0053-function-oriented-adapter-authoring
- **创建日期**：2026-09-14
- **文档状态**：Completed
- **关联分支**：`docs/framework-authoring-rfcs`；建议实施分支 `refactor/adapter-authoring`
- **目标版本**：下一次投产前开发接口版本
- **负责人 / 作者**：LLM-EdgeFlow contributors
- **设计基线**：`3fb4ba5be18f00fd8855b7d2de900e80ad203335`
- **关联决策**：补充 RFC-0044、0048、0049、0050、0052；取代 RFC-0048 中通过 Entity Adapter 复用载体的内部实现选择，保留其外部 JSON 契约。
用户要求形成三份可逐步实施的 RFC；先实施本篇（已交付），再实施
[RFC-0054](0054-controlled-configuration-snapshots.md) 和
[RFC-0055](0055-traceable-batch-operations.md)。三篇可以独立交付，本篇不依赖后两篇。
公共机制保留在各自所属层；本篇不会让 Integration 引用 `nodes/`。
开发与交付流程遵循 [CONTRIBUTING](../../CONTRIBUTING.md)。

## 1. 问题、目标与范围

### 1.1 当前重复工作

| 现有基础或重复点 | 源码依据 | 本次处理 |
| --- | --- | --- |
| Keyword 与 Entity 的 Unpack 重复 envelope 校验、文本复制、批内编号、原始 ID 保存和发布 | [keyword_match_adapter.cpp](../../src/adapter/biz/keyword_match_adapter.cpp)、[entity_extract_adapter.cpp](../../src/adapter/biz/entity_extract_adapter.cpp) | 提取类型化批次执行骨架和载体转换 |
| Translate 为复用文本载体，查找 Entity Adapter 并构造临时 AlgContext | [translate_adapter.cpp](../../src/adapter/biz/translate_adapter.cpp) | 直接组合中立载体组件与业务转换函数 |
| C 数组与 owned Result 已共用 PackTyped | [result_packing_adapter.h](../../include/adapter/result_packing_adapter.h) | 保留双表示行为，继续复用，避免再建 Pack 分派体系 |
| 按请求校验来源已有 IndexResults；DocQA 等仍逐路读取、建索引再拼接 | [result_validation.h](../../include/adapter/result_validation.h)、[doc_qa_adapter.cpp](../../src/adapter/biz/doc_qa_adapter.cpp) | 增加多路一对一结果视图，调用现有校验 |
| Operator 已有槽位描述、shadow storage 和单槽位 builder；转换函数仍手写查表和类型擦除 | [operator_biz_bridge.h](../../include/adapter/operator_biz_bridge.h) | 增加有限的类型化 builder，并共享 Entity/Translate 载体转换 |

目标用户只需理解完整外部请求/响应、普通结构体、单样本转换函数及业务端口声明。
同一项业务规则不能分别在 C Pack、Operator、Demo 中维护。脚手架可以消费新接口，但扩大
模板不作为公共机制的主要实现方式。

### 1.2 必需交付

1. 类型化输入批次骨架：校验、copy-in、编号、局部批次构造及发布。
2. 一份作者声明生成 Adapter 描述与运行时绑定，复用现有注册表和 IBizAdapter。
3. 多路一对一结果对齐，以及固定 C 输出和 owned Result 的共同业务编码入口。
4. Entity/Translate 单槽位 Operator 转换的类型化包装和载体复用。
5. Entity、Translate 输入输出迁移；DocQA 多路结果打包迁移；配套轻量契约夹具与指南。

首期支持外部一请求对应一响应、每路内部结果每请求一项且 `sub_id=0` 的场景。
关键字 Adapter 作为差异对照，是否整体迁移不作为结项条件。ranked 聚合、OCR 多槽位输入、
新的平台结构、任意字段映射 DSL、自动生成业务 JSON schema、通用 Demo 重构均不在范围内。
现有完整 IBizAdapter/Operator bridge 路径继续可用。

## 2. 所属层与接口组织

全部运行时新增代码属于接入适配层 / Integration。C ABI 入口、Operator 输出池及调度保持
既有责任；Core 不知道具体载体或业务字段。建议新增以下源扩展文件，实际拆分可按实现体量调整：

| 文件 | 职责 |
| --- | --- |
| `include/adapter/adapter_authoring.h` | 作者入口及窄的声明工厂 |
| `include/adapter/adapter_result.h` | 类型化转换结果、错误码与 Adapter 诊断 |
| `include/adapter/adapter_batch.h` | typed 输入批次与多路结果读取/对齐模板 |
| `include/adapter/text_carrier.h` | 现有文本载体的 owned 值、转换和输出 writer |
| `include/adapter/operator_biz_bridge.h` | 扩展类型化单槽位 builder，继续维护既有描述符 |
| `src/adapter/` 下对应 `.cpp` | 非模板机制实现；不落入 `src/adapter/biz/` 的某个消费者 |
| `tests/support/adapter_harness.h` | 测试专用资源管理与契约场景 |

这些是 C++ 源扩展头，不加入 C11 公共头。遵循
[头文件边界](../dev_guide/source_layout.md)，保持六个 `Alg_*` 的 `noexcept`、
`catch (const std::exception&)`、`catch (...)` 屏障。

### 2.1 作者声明与普通函数

拟议的最小文本路径由四项组成，下面是接口形状而非可复制编译的完整 API：

```cpp
AdapterResult<std::string> DecodeRequest(const OwnedTextRequest& request);
AdapterResult<std::string> EncodeResponse(const std::string& answer);

// OneToOneTextAdapterSpec:
// identity + existing carrier + typed ingress/egress + decode/encode functions
```

- `OwnedTextRequest` 含外部 `uint64_t request_id` 与 owned `std::string text`。
  carrier 完成外部指针/文本校验和复制；DecodeRequest 决定 text 的业务解释。
- EncodeResponse 仅借用当前结果的普通文本，不接收未经校验的来源编号；其借用仅覆盖调用。
  provenance 校验由后续结果对齐阶段执行，编码函数不能据 req_id 索引其他行。
- `AdapterResult<T>` 持有成功值，或独立的 `return_code` 与完整 `AdapterStatus`，后者保留
  Code、AdapterName、field/index/message 等既有内容。函数返回码与诊断码不能假设相等。
  它属于 Integration，不复用 `NodeResult`，不调用 `AlgContext::SetError`。
- Spec 显式保存业务枚举、Adapter 名、ABI 版本、C 类型名、批次上限、ownership/thread/cardinality、
  `BizDefinition` 的允许名称，以及带类型的 ingress/egress 声明。不能从 C 类型名推断 payload schema。
- 同一份端口绑定声明生成 BizDefinition 并驱动读写；不再另写一份字符串 key 列表。
  raw request IDs 仍使用现有 `kRawRequestIds`，由骨架保存和恢复。
- 薄包装继承现有 IBizAdapter 路径，构造函数和 Spec 元数据最终进入 `REGISTER_BIZ_ADAPTER`。
  不增加中心业务 switch、第二张注册表或静态初始化期间的资源加载。

首期文本 Spec 用于 Translate 的 TextBatch 输入/输出。Entity 复用输入骨架，输出继续走
原 PackTyped 上的 typed 行视图/writer，其视图类型为 StructuredDocumentBatch，业务仍能
检查 JsonDocumentItem；不将其简化为普通文本响应。DocQA 保留自己的输入 Unpack，
仅复用多路结果组件。无需为覆盖这些试点而建设任意结构体反射。

### 2.2 多路结果视图与输出 writer

拟议 `RequestResults<Fields...>` 由每个带类型的结果 key、必需性及来源模式构造。
首期所有迁移字段显式为 required/one-per-request；optional 与 ranked 模式不提供隐式默认值。
框架读取原始 ID 列表和所有结果批次，各路调用 `IndexResults`，完成后按批内请求索引提供只读行视图。

业务编码函数负责字段选择、结构化结果成功状态、响应 JSON 和外部状态映射。输出 writer 负责
既有目标类型的受检赋值：固定数组使用现有 `CheckedStringCopy`，owned string 使用变长写入。
多字段业务保持原有字段写入次序，不先经过固定 C DTO 再生成 owned Result。
required 缺失时的返回码和 AdapterStatus 由迁移绑定显式沿用原业务，不能顺手统一成新错误码。

文本 Spec 的 Decode/Encode 返回文本载荷；多字段路径使用普通 typed 行视图和 writer 回调，
无需让作者访问 `AlgContext` 或 `void**`。复杂业务继续使用原 PackTyped 扩展点。

### 2.3 首期输出阶段与兼容绑定

输出入口允许一个显式的、仅处理 owned 批次的 PrepareResults 回调；它是业务转换阶段，
不发布 Context，也不触碰外部输出。首期 Translate 使用该阶段，Entity/DocQA 不使用。

| 试点 | 确定的执行顺序 |
| --- | --- |
| Translate | 先按原次序读取 answers/IDs；对全部 answers 调用 EncodeResponse 序列化 JSON；再按原 carrier 的次序做容量/来源检查；最后写 ID、status 和 JSON |
| Entity | 保留原先必需结果读取、容量/来源检查；逐行先写 request_id，再检查结构化状态，通过后写 status 和 JSON |
| DocQA | 保留原必需值/槽位预检顺序，建立三路 typed 索引，再按原字段顺序写输出 |

Translate 的全批序列化先于容量/来源检查，保留无效 UTF-8 等序列化失败相对于其他错误的
优先级。Entity 不提前执行全批业务成功状态检查；失败样本的 request_id 可能已经修改，
而该样本的 status/text 仍保留原值。PrepareResults 是有限的阶段回调，不建设可配置执行图。

迁移绑定保留现有诊断来源：例如 Translate 缺 answers/IDs 时返回码为 INVALID_INPUT，
底层读取器写入的 AdapterStatus.Code 却是 BUFFER_TOO_SMALL；原 Entity carrier 产生的
部分诊断仍标为 EntityExtract。包装按阶段转发完整状态，不用 Spec 名称统一重建诊断。
删除对 Entity 注册实例的依赖后，“找不到 carrier 实例”分支自然消失；其余现存输入、输出
及 Context 错误按特征测试保留。未来统一诊断身份时需另列兼容变化。

## 3. 执行、所有权与失败契约

### 3.1 输入顺序

1. 公共 Runtime 原有 `ValidateBatch` 在模型执行前完成槽位数量、批次上限等预检。
2. 直接 Unpack 仍执行其防御性输入检查；复用 `AdapterValidationHelper`，不假设调用者一定经过 C ABI。
3. 先完成整批 carrier 校验与 copy-in，再逐样本调用业务 Decode，构建内部 owned 批次。
   不能交错为“第 0 项 carrier→Decode、第 1 项 carrier→Decode”，否则会改变 Translate
   前项 JSON 错误与后项载体错误同时存在时的诊断优先级。
4. 第 i 个输入产生批内 `req_id=i`、`sub_id=0`；外部 ID 原样保存在独立列表。
   外部 ID 重复不是内部 provenance 重复，不额外拒绝。
5. 全部样本转换成功后才逐 key 发布。业务校验失败不发布转换批次、不执行 Pipeline。

现有 `AlgContext` 没有多 key 事务；若直接调用 Adapter 时后一个 key 重复，先前已成功的
Publish 不回滚。公共 Runtime 每次创建新 Context，Unpack 失败即退出并丢弃它。
本篇不修改此行为，也不承诺直接 Unpack 的多 key 原子性。

### 3.2 输出顺序与兼容表

输出阶段顺序按第 2.3 节的迁移绑定执行；公共检查和逐字段 writer 保持下表行为。
`req_id` 只用于定位批内样本，外部 ID 从原始列表恢复；乱序结果允许，重复、缺失、越界和
一对一路径的非零 sub_id 继续失败。

| 情形 | 必须保持的行为 |
| --- | --- |
| 槽位容量不足，或 outputs 为空且 count 指针有效 | 原检查器回填所需数量并返回容量错误；不得统一清零 |
| count 指针为空、负容量或某个输出槽为空 | 保留现有返回码与 count 更新规则，使用原检查器 |
| 固定 C 字符串容量不足 | CheckedStringCopy 可能写入截断文本及结尾 NUL 后报错；先前字段/样本不回滚 |
| 普通字段/来源失败 | 保留原业务返回码、AdapterStatus 和 count 行为；不把失败 count 改成成功数量 |
| 成功 | 最后写入实际输出数量 |
| owned Result | 直接写入变长字段，复用相同来源/业务检查；没有固定 C 字符数组上限，也不承诺直接 PackResultBatch 回滚 |
| Operator 外部输出 | 继续在全部转换成功后发布；失败由现有 lease guard 回收，不能提前暴露部分输出 |

各业务的错误检查次序也可能决定诊断。本轮先以特征测试锁定迁移对象，再要求新骨架匹配；
确需调整的外部错误契约必须在本 RFC 明列变化后实施，不能作为“顺手修复”。

### 3.3 文本与临时对象

保留现有载体的 C 字符串终止、长度、NUL 和 copy-in 规则。CompanyString 到 C 载体的转换
继续使用已有 ValueType 校验与稳定 storage。JSON 解码后的字符串可包含转义产生的 NUL，
不能重新通过 `strlen` 选取模型输入。Translate 编码使用 JSON serializer 正确转义，不解析、
修复或裁剪模型返回的普通译文。

转换函数不保存借用输入、结果视图、writer 或 storage 到下一次请求。Spec 仅保存声明和
无捕获函数，不保存请求状态。可共享的 carrier 代码不意味着 Entity 与 Translate 共享业务 schema。

## 4. Operator 类型化包装

首期为 Entity/Translate 提供 typed 单槽位 builder，输入回调接收已选定宿主类型的 const 引用
和现有 `ProcessLocalShadowStorage`，输出回调接收 owned Result 引用、宿主输出引用及
`ResolvedOutputPoolSpec`。原 `MakeSingleSlotBizBridge` 和描述符可继续作为底层实现。

1. 类型化 traits 在声明处绑定现有宿主 C++ 类型与已注册 suffix；统一槽位声明和转换函数的类型。
2. key 格式、必需/未知槽位、空值、ValueType 校验仍由现有 binding 过程先执行。
3. `void*` 擦除仅在包装边界发生。外部错误指针的真实动态类型无法靠字符串类型名可靠识别，
   builder 的保证是减少作者接线错误，不是任意 C 指针的运行时类型安全。
4. shadow DTO 和字符串仍由一次 Process 的 storage 持有；禁止返回局部 string 的 `c_str()`。
5. 输出容量只消费已解析的 pool spec，不重新读取配置；沿用 RFC-0049 的租约、回滚和多输出发布。
6. 一个生产 Adapter 仍必须有匹配 bridge，通过现有 GlobalInit 完整性检查。

复用组件按载体命名，不按 Translate/Entity 业务命名。多槽位与多输出路径保持完整接口，
不将单槽位 builder 扩张为任意字段映射语言。

## 5. 试点与兼容迁移

| 试点 | 迁移内容 | 必须证明 |
| --- | --- | --- |
| Entity | 文本输入骨架、原 PackTyped 上的结构化结果行视图/writer、typed bridge | 原始文本契约、JSON 结果状态、C/owned 输出一致 |
| Translate | 独立载体 + Decode/Encode；删除对 Entity Adapter 的运行时查找和两个临时 Context | 完整 JSON 请求直达 SDK；只选择 query；响应仍为 translated 对象 |
| DocQA | 仅将多路一对一结果读取/对齐收进组件 | 回答、意图类别/置信度、分块数和状态的 required/容量语义不变 |

保留 biz_name、C 结构布局、枚举、Pipeline JSON、Descriptor 元数据和已有 Operator key。
新组件不要求 Catalog schema 变更。迁移前后以对应构建的 Catalog 比较契约投影；首次实施先
重建工具，执行 `./build/alg_pipeline_tool catalog`，不能用旧二进制确认新源码的注册情况。

本篇仅取代 RFC-0048 的内部 carrier 复用方式，不改其输入输出决策。可按试点逐个回退到原
IBizAdapter 实现；共享组件仍被其他试点使用时保留。不会产生双重注册或按配置选择新旧契约。

## 6. 验证与可观察验收

扩展现有套件，不新增独立测试可执行文件：

| 所有者 | 最小证明 |
| --- | --- |
| [Adapter purity](../../tests/unit/adapter/test_adapter_purity.cpp) | copy-in；外部重复 ID；全部样本校验后发布；后续 key 冲突的现行部分发布行为；多路乱序、缺失、重复、越界 |
| [ABI security](../../tests/contract/abi/test_adapter_contract_security.cpp) | 完整 Translate JSON 原始请求调用 Alg_Process；非对象/缺 query/类型错误；引号、换行、Unicode、JSON 转义 NUL；固定/变长输出及长结果；跨样本载体/业务错误优先级；返回码与完整 AdapterStatus 独立保持 |
| [C ABI safety](../../tests/contract/abi/test_c_abi_safety.cpp) | 负/零/上限批次的既有行为；null/count/槽位预检；字段失败后的前序写入和 count 保留；Entity 失败样本 ID/status/text 哨兵值 |
| [Bridge registry](../../tests/unit/operator/test_operator_biz_bridge_registry.cpp) | 声明不匹配、缺 bridge、未知槽位及类型 suffix；typed builder 仍走同一校验 |
| [Operator API](../../tests/integration/operator/test_operator_api.cpp) | 输入引用不留存；短字符串地址稳定；长输出失败回收后重试；共享载体不合并 payload schema |

`AdapterHarness` 仅集中 Context/句柄/缓冲区所有权、来源扰动和两种输出调用机械设置。
作者提供原始请求、独立人工期望、内部结果注入及字段观察器。只有声明为一对一的场景才能
使用一对一测试集合；局部 harness 不替代原始请求的 Alg_Process 契约测试。

迁移后验证受影响 Pipeline 的 validate/plan，运行对应 Demo-supported 路径并检查结果；
Translate 的 C ABI 证据不以 Demo 成功代替。企业内部 SDK 与硬件不属于此中立重构验收，
继续遵守 [RFC-0029](0029-external-readiness-and-intranet-sdk-migration.md)。

体验验收任务：在已有文本载体上增加一个不同 JSON 字段契约，仅编辑普通转换函数、单份声明
和独立期望即可完成；不需要理解黑板读写、批内编号或 shadow 存储实现。记录修改位置、求助
原因和失败定位；工程验证与真实开发者试用分别记录，不以代码行数宣称体验验收通过。

## 7. 实施步骤与阶段出口

| 阶段 | 实施内容 | 出口条件 |
| --- | --- | --- |
| M0 基线 | 获取新构建 Catalog；锁定 Entity/Translate/DocQA 的正常与失败行为；记录现有修改步骤 | 特征测试覆盖 count、部分写入和来源；现有测试通过 |
| M1 载体与骨架 | 实现 AdapterResult、owned 文本 carrier、批次校验/构造/发布 helper；暂由原 Adapter 调用 | 不引入新业务注册；异常/所有权测试通过；无跨层依赖 |
| M2 作者接口试点 | 提供单路视图/writer；单份 Spec 衔接原注册；迁移 Entity、Translate；删除跨业务 Adapter 查找 | Catalog 契约不变；原始 JSON C ABI 验收通过 |
| M3 多路结果 | 扩展 required 多路一对一对齐；复用 writer 迁移 DocQA 打包 | 乱序和失败矩阵通过；C 与 owned Result 无固定容量串接 |
| M4 Bridge 与测试入口 | 类型化单槽位转换、共享载体；提取最小 AdapterHarness | GlobalInit 完整性、生命周期、池回收测试通过 |
| M5 文档与交付 | 更新业务 onboarding、编译示例、必要 Changelog；完成受影响方案执行与体验记录 | 按 CONTRIBUTING 完成一次 canonical gate；如实记录试用完成或待验收 |

每阶段可以独立评审，后续实现先确认前一阶段出口。ABI/所有权变化由独立 Reviewer 检查。
可选脚手架和其他 Adapter 全量迁移不是必需出口；不得用它们拖延或替代上述试点。
没有真实试用记录时保留相应待办，不把框架作者自测记作新手验收。

## 8. 实施与最终结果记录

| 项目 | 当前状态 |
| --- | --- |
| 设计文档 | Completed；接口与迁移规格已形成并交付 |
| 生产实现与试点 | 已交付；完成 `AdapterResult`、`text_carrier`、`adapter_batch` 与 `adapter_authoring`；Translate 全面迁移至 `OneToOneTextAdapter` 并删除 Entity 动态查找；EntityExtract 迁移至 typed batch 与 text writer；DocQA 迁移至多路结果对齐；完成 typed Operator bridge 与 `AdapterHarness` |
| 工程验证 | 已通过；`./scripts/run_all_tests.sh` 包含 97 项测试全部通过，覆盖静态源码检查、CMake 配置构建、全量 CTest、架构层依赖与隔离检查以及 Catalog 契约一致性校验 |
| 开发者试用 | 待记录；体验任务规格已就绪，保持试用待办，不以框架作者自测替代实际新手验收 |
| 完成条件 | M0–M5 必需交付、契约验证与现行指南已完成；体验验收任务设计就绪，待后续真实开发者试用后补充记录；文档状态更新为 Completed |

实施进展和最终差异直接更新本文；生产实现与工程验证已在 `refactor/adapter-authoring` 分支交付。
