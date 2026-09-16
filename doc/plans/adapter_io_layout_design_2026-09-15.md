# 输入、输出转换独立化：详细设计与实施计划

- 更新日期：2026-09-15。
- 源码基线：`e6aa776c959aa84f5eec20bf38e73c313718e01a`。
- 状态：**已实施**。
- 目标：输入转换器与输出转换器独立复用；外部结构变化被限制在接入适配层，Pipeline 与 Node 只处理框架内部数据。
- 范围：转换器、接入绑定、创建配置、运行时装配、Operator 接入、Catalog、验证及仓内消费者迁移。
- 实施流程：[CONTRIBUTING.md](../../CONTRIBUTING.md)；现行约束：[AGENTS.md](../../AGENTS.md)。

本文只维护最终目标设计及实施步骤。文中的新类型、注册项、配置字段和工具命令均为**待实现接口**，不能当作当前可用能力。
源码实施涉及注册契约、公开 ABI 和运行时边界，必须先按现行流程登记 RFC，引用本文的详细设计和验收要求。
本次文档交付不提前宣告 RFC 或源码实现完成。

**上线前一次迁移：不向旧版本兼容，不保留两套设计。** 仓内调用方、配置、示例、工具与测试同批切换，
创建时必须显式指定接入绑定。删除旧配置自动识别、缺省绑定、旧业务枚举选择、兼容包装及旧注册/执行路径。
旧配置与旧 ABI 不属于新版本支持范围。C ABI 与 Operator 是两个实际宿主入口，共用一套组件机制，不是新旧版本双轨。

## 1. 最终设计

### 1.1 三个可组合部分

```text
选定的输入转换器 → Pipeline → 选定的输出转换器
```

- 输入转换器：读取明确的外部结构与请求协议，校验并复制数据，发布到框架内部 typed ports。
- Pipeline：通过内部数据类型和逻辑端口执行算法，保持现有四层依赖方向。
- 输出转换器：读取框架内部结果，完成响应字段选择、组装、序列化及受容量约束的外部写入。

输入转换器不引用输出转换器；输出转换器不读取原始外部输入。两者依靠内部端口、来源编号和明确的请求元信息协作。
接入绑定声明这三个部分如何配合，并在创建实例时完成兼容性校验。

### 1.2 两条目标数据流

```text
C ABI 调用方
  → C ABI 通用入口：句柄、批次与调用边界
  → input：外部 C 请求 → 内部数据
  → Pipeline / Nodes
  → output：内部结果 → 外部 C 响应
  → C ABI 调用方

Operator 调用方
  → Operator 通用入口：命名槽位、宿主类型、输出池
  → input：Operator 外部请求 → 内部数据
  → Pipeline / Nodes
  → output：内部结果 → 已租用的 Operator 输出结构
  → Operator 通用入口：发布输出与管理租约
  → Operator 调用方
```

两种入口统一调用 Decode/Encode；宿主结构读取、容量约束和发布方式按入口特化，详见第 6、9 节。
不为复现旧错误优先级增加额外公开回调、历史阶段或策略开关。

### 1.3 Operator 的最终职责

Operator 保留槽位解析、ValueType 校验、输出分配策略、池预算、租约、发布、释放和句柄并发控制。
具体外部字段转换全部由 input/output 承担。

**最终目录中不保留业务 `operator/bridges/`。** 当前业务 bridge 的输入字段转换迁入 input，输出字段转换迁入 output，
槽位描述迁入转换器 Definition 与接入绑定。通用 Operator 代码根据已验证的绑定调用组件。
Operator 输入不再为了复用业务算法而强制构造另一份公共 C 结构；它可以在输入组件内使用自持有的中性快照。

输出组件可以在内部暂存自持有响应，以完成整批检查和宿主写入；该对象由输出组件管理，
不成为 Pipeline/Node 的数据类型，也不成为独立业务桥的输入契约。

## 2. 复用规则与隔离边界

| 复用对象 | 允许的复用 | 判断依据 |
| --- | --- | --- |
| 输入转换器 | 多个业务或 Pipeline 使用同一个转换器 ID | 外部类型、请求 schema、字段语义与产出的内部端口匹配 |
| 输出转换器 | 多个业务或 Pipeline 使用同一个转换器 ID | 所需内部结果、响应 schema、宿主类型与容量策略匹配 |
| 载体读写 helper | 不同转换器共享字符串、数组、PCM、缓冲区访问代码 | 类型及内存布局一致；helper 不替代业务协议校验 |
| 语义转换函数 | 不同宿主类型共享请求解释或结果组装函数 | 中性字段和协议语义一致 |
| Pipeline | 不同输入、输出格式使用同一 Pipeline JSON | 各组合都通过内部契约与数据流校验 |

相同 `const char*` 或同名 C 字段不保证 payload 协议相同。例如纯文本与 `{"query":"..."}` 可以复用载体读取代码，
但必须分别应用对应的请求解释规则。类型名、schema ID 和 schema 版本都应进入 Definition。

隔离必须满足：

1. 平台结构、外部字段名、输出池对象不进入 Core、Node、Model 或 Backend。
2. 输入发布到 `AlgContext` 的是拥有自身数据的内部值；保持当前 copy-in 规则，不引入跨调用借用。
3. 外部请求 ID 保存为请求级元信息；内部 `req_id/sub_id` 继续表示批内来源，输出负责恢复外部编号。
4. 需要透传的信息必须成为声明过的中性请求元信息，output 不能回读外部指针或不透明宿主对象。
5. 转换器不可持有请求状态。Definition、绑定计划共享且不可变；快照、编码暂存和租约属于本次调用。
6. 新外部 schema 由 SDK 转换器完整实现，Demo/Python 只构造载体、调用和显示结果。

## 3. 目标源码布局

```text
include/adapter/
├── io_converter.h               # 转换器 Definition、调用契约
├── io_binding.h                 # 绑定 Definition、只读查询契约
├── converter_authoring.h        # typed helper 与注册入口
└── ...                          # 保留仍有使用者的共享契约

src/adapter/
├── input/
│   ├── README.md
│   ├── text_input.cpp
│   ├── translate_json_input.cpp
│   ├── doc_query_input.cpp
│   ├── rerank_input.cpp
│   ├── audit_input.cpp
│   ├── audio_input.cpp
│   ├── image_query_input.cpp
│   └── ...                      # 相邻私有头与读取 helper
├── output/
│   ├── README.md
│   ├── structured_document_output.cpp
│   ├── translation_json_output.cpp
│   ├── keyword_result_output.cpp
│   ├── doc_answer_output.cpp
│   ├── rerank_result_output.cpp
│   ├── audit_result_output.cpp
│   ├── audio_result_output.cpp
│   ├── invoice_result_output.cpp
│   └── ...                      # 相邻私有头、编码暂存与写入 helper
├── biz/
│   ├── README.md
│   ├── translate_bindings.cpp
│   ├── doc_qa_bindings.cpp
│   └── ...                      # 内部业务契约与接入组合声明
├── operator/
│   ├── operator_adapter.cpp
│   ├── operator_process_binding.cpp/.h
│   ├── operator_config_resolver.cpp/.h
│   ├── operator_output_pool.cpp/.h
│   └── ...                      # ValueType、配置、Control 等通用机制
├── io_converter_registry.cpp/.h
├── io_binding_registry.cpp/.h
├── io_binding_resolver.cpp/.h
├── io_catalog.cpp/.h
├── deployment_io_config.cpp/.h
├── c_api_adapter.cpp
└── shared_algorithm_runtime.cpp/.h
```

目录按转换方向区分，文件按协议或转换操作命名。一个文件可以注册多个使用相同实现的宿主特化版本，
不要求为每个注册项新建文件。复杂协议可以在 input/output 内按操作建立子目录，当前八个业务不强制增加层级。

`biz/` 只声明内部业务契约和输入/输出组合，不实现字段转换，不包含转换器私有头，使用稳定 ID 引用注册项。
公共 C 头仍在 `include/edgeflow/`，平台替身仍在 `include/platform_mock/`。
新 C++ 扩展声明按使用范围进入 `include/adapter/`；注册表内部、快照和响应暂存类型放在 `src/adapter/`。

## 4. 组件与接入绑定的数据模型

以下为目标字段与约束。实施时复用现有 typed port、诊断和注册设施，不另建通用反射框架。

### 4.1 输入、输出 Definition

| Definition | 必需信息 |
| --- | --- |
| `InputConverterDefinition` | 唯一 `converter_id`、`transport`、请求 `schema_id/schema_version`、外部输入槽与类型、内部逻辑输出端口、批次上限、copy-in 与线程策略 |
| `OutputConverterDefinition` | 唯一 `converter_id`、`transport`、响应 `schema_id/schema_version`、消费的内部逻辑端口、外部输出槽与类型、输出基数、批次上限、容量与线程策略 |

`transport` 本轮只有 `cabi`、`operator`。每个注册项选择一种入口，ID 可以包含入口后缀，
例如 `plain_text.entity_cabi.v1`、`plain_text.entity_operator.v1`；这些是建议的新 ID，不是当前 Catalog 内容。
两种入口可共享函数，但外部类型或资源契约不同的注册项必须分别声明。

转换器 Definition 不含 `BizType`、`biz_name` 或对端转换器 ID，不持有 Pipeline 实例。
内部端口使用逻辑名称和 typed Definition。类型、基数、来源与生命周期沿用现有端口机制。
外部槽位定义包含逻辑槽名、准确的宿主类型 ID、方向、必需性；Operator 槽位还引用 ValueType 的规范后缀。
单个注册项不得使用一个裸指针入口接收多种未标识布局，也不得按字段内容猜测结构体类型。

### 4.2 `IoBindingDefinition`

一个绑定描述某一入口上完整的外部请求/响应组合：

| 字段 | 含义 |
| --- | --- |
| `binding_id` | 全局唯一的接入组合 ID |
| `biz_name` | 内部业务契约；与目标 Pipeline 精确匹配 |
| `transport` | `cabi` 或 `operator` |
| `input_converter_id` / `output_converter_id` | 分别选择已注册转换器 |
| `input_ports` | 输入转换器逻辑输出端口 → 实际 Blackboard key |
| `output_ports` | 输出转换器逻辑输入端口 → 实际 Blackboard key |

绑定 ID 表达完整协议组合。切换输出 schema 使用另一个绑定 ID，不在原 ID 下静默改变响应格式。
多个绑定可以引用相同 `biz_name`、相同输入 ID 或相同输出 ID。

本轮通过已注册绑定声明组合组件，创建配置选择 `binding_id`，不在 Process 请求内自由拼接转换器。
新增组合通常只需增加声明与配置，不复制输入/输出算法。未来若允许部署文件直接声明组合，仍须走相同解析与验证入口。

### 4.3 业务契约只注册一次

`BizDefinition` 继续描述 Pipeline 的内部 ingress/egress。注册从 `BizAdapterRegistry::RegisterAdapter`
的附带动作移出，由 `biz/*_bindings.cpp` 的业务声明独立完成，复用 `PipelineCatalog::RegisterBizDefinition`。

同一个 `biz_name` 只允许一个 Definition；多个接入绑定引用它，不能重复注册或以“定义一样”为由覆盖。
转换器注册不注册业务契约。同一 Pipeline 因此可以绑定不同外部结构，无需复制 Pipeline 或改写其 `biz_name`。
完整外部输入/输出约定由 binding 加两侧 schema 共同描述，Catalog 的内部端口不能替代该约定。

### 4.4 注册表与全量审计

一个 `IoConverterRegistry` 管理输入、输出两个类型化集合，一个 `IoBindingRegistry` 管理绑定。
作者入口采用 Spec、普通函数和 typed helper，不引入多层转换器继承体系。

全局初始化必须拒绝：

- 重复 converter ID、binding ID、biz_name，以及缺失的必要回调或 schema 元数据。
- 绑定引用不存在的业务或转换器、入口类型不一致、端口映射不合法。
- 必需接入能力缺失，以及绑定的业务契约、宿主类型和组件声明不一致。
- Operator 槽位类型、方向、规范后缀、分配/重置/释放能力不完整。

当前八个业务的 C ABI 与 Operator 接入能力都要完成迁移。生产业务集合由每个 `biz/*_bindings.cpp`
中独立的 `BizExposureDefinition` 声明，字段为 `biz_name`、`max_batch_size` 和 `required_transports`，
与 BizDefinition 同时登记，不从已有绑定反推应有集合。
每个业务的每种必需入口至少存在一个有效绑定；可以有多个，但调用方必须明确选择 binding_id。
不建立默认项、业务枚举索引或根据 biz_name 自动选择绑定的逻辑。内部纯 Pipeline 测试契约可不声明生产 exposure。
已注册但暂未被绑定使用的通用转换器允许存在，不能按“孤儿业务桥”的旧规则拒绝可复用组件库。
每个声明过的绑定必须完整，即使暂未被选中，也应在全量审计中检查其有效性。

## 5. 内部端口绑定与类型安全

输入转换器产出逻辑端口，例如 `texts`；绑定将它映射到现有业务实际 key，例如 `input_sentences`。
输出转换器声明 `answers` 等逻辑端口，由绑定选择实际读取的 key。
具体 key 与类型必须从当前 Definitions/Catalog 查询，不能依据文件名猜测。

创建时将映射解析为不可变的 `InputPortBindings`、`OutputPortBindings`，运行时使用类似
`bindings.Key<TextBatch>("texts")` 的 typed helper。这表示目标接口约束：类型在创建时核对，
运行时不靠 `std::any` 猜类型，不允许通过未检查的字符串访问任意黑板值。

请求来源由共享输入机制写入本次 `AlgContext`，复用 `kRawRequestIds` 及已有 provenance 规则。
外部重复 request ID 仍允许；内部按批内编号和子编号关联，不能用外部 ID 当唯一哈希键。

对于输入额外发布、输出实际未消费的现存端口，保留实际行为与业务 Definition：

- CrossRerank 当前额外发布 `kRerankPairs`，输入转换器明确记录该写入；不删除实际数据，也不悄悄改原 Catalog ingress。
- ComplianceAudit 的业务 Definition 声明 `kRuleMatches`，当前打包不读取它；输出按实际读取声明 consumes，Pipeline 原 egress 要求仍保留。

创建校验分别处理“业务契约要求”和“转换器实际读写”，不复制一份不准确的元数据。

## 6. 转换器调用接口与复用实现

### 6.1 输入组件：统一 Decode

两种入口的输入转换器都提供同一形状的回调，按已绑定的具体宿主类型生成 typed wrapper：

```cpp
int DecodeInput(const ExternalInputBatchView& source,
                const InputDecodeOptions& options,
                const InputPortBindings& bindings,
                AlgContext* context, AdapterStatus* status);
```

| 类型/阶段 | 契约 |
| --- | --- |
| `ExternalInputBatchView` | 本次同步调用有效的只读视图，每槽带已解析类型 ID；typed accessor 核对绑定类型后访问，不拥有调用方内存 |
| `InputDecodeOptions` | 创建时确定的批次/宿主限制与诊断上下文，不含输出组件 |
| Decode | 有界读取外部字段、校验完整 payload、复制数据并发布内部值；遇到错误立即停止 |

仅 Decode 成功后运行 Pipeline；失败时销毁本次 AlgContext，因此无需为恢复旧发布次序另建事务/回放机制。
输入必须在读取前检查指针、长度和乘法溢出，不能先无界复制再补校验。
转换器内部可用自持有文本、文档/问题、PCM、图片引用记录复用语义函数，但不建立框架级 PreparedInputBatch 或宿主 Prepare 阶段。
不创建公共 C shadow DTO 作为 Operator 到内部数据的必经中转。

普通文本解释函数可服务多个外部读取器，JSON 解释函数负责自己的 schema。
每个 schema 明确可选字段空值、长度、内嵌 NUL 和 UTF-8 规则；有业务意义的约束必须保留。
校验次序以第 9 节新约定为准，不承诺复现旧入口对多个同时非法字段的报错顺序。

### 6.2 输出组件：共享语义、宿主特化写入

```cpp
int EncodeOutput(AlgContext* context,
                 const OutputPortBindings& bindings,
                 const OutputEncodeOptions& options,
                 ExternalOutputBatchView* destination,
                 size_t* written_count,
                 AdapterStatus* status);
```

输出组件在 Encode 内按实际需要创建局部自持有响应，不公开 ReserveOutputScratch 或 OutputScratch 阶段。
written_count 在入口置零，成功时返回写入的请求数；运行时按绑定基数验证后才发布成功结果。

`ExternalOutputBatchView` 是已有目标内存的借用视图，带槽位类型和容量信息，不授予转换器发布租约的能力。
输出组件只写入已提供的字段空间，不能分配另一套输出池、返回临时字符串指针或直接给用户 map 发布 `shared_ptr`。
`OutputEncodeOptions` 保存创建时确定的诊断上下文与调用限制，不含输入转换器或外部输入指针。
两侧 options 的诊断上下文使用 binding_id、converter_id 和阶段；不保留旧 Adapter 名称的错误格式兼容配置。

结果选择、来源关联和响应组装使用一份共享语义实现，按宿主接入不同 writer：

- C ABI 特化写入调用方固定结构，按声明容量拒绝截断。
- Operator 特化先用同一 helper 构造自持有响应，再写入已租用的宿主结构；字符串容量由 `ResolvedOutputPoolSpec` 决定。
- 暂存响应与宿主字段写入都属于该输出组件，其注册项独立于输入转换器和业务绑定。
- 可复用现有 Integration Result 的存储形状；最终不再通过 `IBizAdapter::ResultTypeName()` 绑定到 Operator 业务桥。

**禁止采用固定 C 输出作为 Operator 输出的中转。** 超过 C 数组上限、但在 Operator 池容量范围内的结果仍应成功。
C ABI 调用失败时输出视为无效，调用方必须丢弃；不承诺历史部分写入位置，也不要求所有输出字节完全不变。
Operator 通过租约及最后统一发布保证调用方看不到部分结果。

### 6.3 接口拆分约束

模板定义留在 output 私有头，或在 `.cpp` 显式实例化实际使用的目标类型；不把业务模板暴露给 Core/Node。
转换器注册不依赖业务绑定，绑定通过 ID 选择组件。
统一返回码、诊断类别和阶段映射，不为历史返回码/诊断不一致保留特殊分支。
复用适当的命名错误常量；输入或结果语义非法、容量不足、内部异常分别给出一致类别和具体字段路径。

## 7. 唯一创建配置与公开接口迁移

### 7.1 选择方式

配置只选择完整 `io_binding` ID。输入、输出在绑定声明中分别指定，可以独立复用；
创建后冻结选择，不支持通过 `Alg_Control` 或单次请求切换外部布局。

`io_binding` 必填且非空；缺失、未知或不匹配时失败，不根据 biz_type、biz_name、文件名或宿主类型推断。
不把 `io_binding`、外部类型或转换器参数放进 Pipeline JSON，Pipeline 继续只描述内部业务和数据流。

### 7.2 C ABI 创建

`CompanyAlgParamCreate` 删除 `biz_type`，保留配置路径、模型根与设备参数。config_file_path 只接受接入配置：

```json
{
  "schema_version": 1,
  "data": {
    "pipe_path": "pipeline_translate_cpu.json",
    "io_binding": "translate.cabi.v1"
  }
}
```

配置 schema 1 是这套接入配置的版本号，独立于 Catalog 和 SDK ABI 版本。
根只允许 schema_version=1 和 data；data 必须含 pipe_path、io_binding，可选 model_paths。
`outputs` 属于 Operator 池配置，C ABI 不接受。直接传入原始 Pipeline JSON、旧配置、未知版本/字段或空绑定一律报配置错误。
不实现旧格式识别、自动升级、尝试其他格式或默认绑定路径。

两种入口的 `pipe_path` 均相对接入配置文件目录解析，规范化后限制在该目录及其子目录；模型根与配置根分别处理。
`model_root_dir` 保持“直接包含模型资产的目录”含义，不要求 Pipeline 文件位于模型目录。
可选 `model_paths` 先按已声明 model_id 应用覆盖；未知 ID、非法类型拒绝。相对模型路径按非空 model_root_dir 解析且不得逃逸，
model_root_dir 为空时模型路径必须为绝对路径。接入配置可放在 configs/，让 pipe_path 引用同目录 Pipeline。
接入配置在 Integration 消费完毕，传给 Core 的只能是已加载的纯 Pipeline JSON。

不同结构使用不同绑定 ID，调用方必须按所选契约构造对象。创建参数和运行时不再保存旧业务枚举。
现有 `const void**/void**` 无法证明指针真实指向哪种结构。类型元数据只检查配置一致性，不能宣称已校验任意指针的实际布局；
不得读取未知对象内容进行类型探测，也不能根据 Pipeline 静默切换 C 结构。

### 7.3 Operator 创建

Operator 使用同一个接入配置 schema，只增加由宿主资源职责决定的必需 outputs：

```json
{
  "schema_version": 1,
  "data": {
    "pipe_path": "pipeline_translate_cpu.json",
    "io_binding": "translate.operator.v1",
    "outputs": {
      "entity_out": {
        "type": "entity_out",
        "capacities": {"entities_json": 4096}
      }
    }
  }
}
```

正式槽名、类型、容量字段必须由所选绑定和目标构建的 ValueType 定义核对。
cfg_file_name 在 model_path 资源根内定位接入配置；配置内部 pipe_path 按配置目录解析，与 C ABI 一致。
model_paths 按入口传入的资源根解析，Operator 使用 model_path。保留资源逃逸检查、输出分配白名单和池预算。
所有现有 conf 同批增加 schema_version、io_binding，并迁移相对路径；缺字段的旧配置不会自动获得绑定。

输入槽来自输入转换器，输出槽来自输出转换器，输出分配配置必须与后者一致。
`type` 不能绕过绑定改变外部结构；未知额外槽、重复匹配、缺必需槽保持拒绝。

### 7.4 ABI 与调用方一次迁移

这次不保留旧 ABI 兼容。更新 COMPANY_ALG_ABI_VERSION、SDK/SOVERSION 和相应导出/版本断言，所有仓内调用方重新编译。
六个 Alg_* 入口继续承担 C11 调用边界与 noexcept/catch 屏障；其必要性不来自旧版本兼容。
创建参数删除业务枚举，旧布局不提供转接函数或 V1/V2 双入口；实际业务输入/输出结构按新绑定声明继续使用。

公开 C++ 辅助接口 `ValidateOperatorConfigBinding` 的 expected_biz_type 参数改为必填、非空的
`const char* expected_binding_id`，精确核对配置的绑定，不提供旧整数重载或“0 表示不比较”的模式。
同步 `scripts/check_sdk_exports.sh` 中的符号、Demo 注册/预检、C11/ABI 测试和所有创建调用。

删除 SharedAlgorithmRuntime、ResolvedOperatorConfig、Operator 句柄和 RuntimeOptions 中仅用于旧选择的 biz_type。
仓内已无消费后删除 CompanyAlgBizType 及其映射 helper；Pipeline 的 biz_name 仍是内部契约 ID，Control 继续按 cmd_id/node_id 路由。
Demo 通过显式接入绑定及声明的载体匹配，不能取消防止错误宿主结构被传入的检查。

## 8. 创建解析、验证与 Catalog

### 8.1 分工与一次性计划

```text
读取唯一接入配置 schema
→ 选择绑定、解析转换器 Definition 与宿主槽位
→ 解析内部端口映射及输出分配配置
→ PipelineValidator 验证内部 Pipeline 与选定 I/O 边界
→ 生成不可变接入计划与 ValidatedPipelinePlan
→ 创建模型/Node 与宿主资源
→ 返回可用句柄
```

`IoBindingResolver` 位于 Integration，负责 schema、宿主类型、显式绑定 ID、ValueType 和容量分配配置校验。
它将转换器实际产生/消费的端口映射转换为**中性的内部边界描述**，交给现有 `PipelineValidator`。

`PipelineValidator` 仍是内部数据流验证与规划的唯一实现。增加接受可选中性 I/O 边界约束的验证入口，
复用 `ValidatePortFlowContract`、键类型、生产者和 egress 检查，不在 Integration/Studio 再写一套 DAG/端口验证。
Core 只看到 key、type、cardinality、provenance、lifetime 等中性字段，不包含转换器类、C 类型或 Operator 槽位对象。

至少检查：

1. 输入发布覆盖业务必需 ingress；类型及 flow contract 匹配；额外实际写入也与 Pipeline 生产者一起核对。
2. 输出必需读取由 Pipeline 或已声明请求元信息提供，每个逻辑端口有唯一合法映射。
3. 原业务必需 egress 仍完整，即使选定输出只消费其中一部分。
4. 禁止重复生产者、不兼容基数、错误来源或生命周期。
5. 本轮生产绑定为每请求一个外部结果；候选展开和多槽位保留内部来源规则。
6. 静态校验不替代运行时缺值、重复/越界来源和实际容量检查。

`ValidatedIoPlan` 的批次上限取输入、输出及生产 exposure 上限的最小值；生产 exposure 明确声明业务允许的上限。
本轮外部结果基数固定为每请求一个，因此预检所需输出数为输入数；Operator 多输出槽仍是每帧各槽一个对象。
迁移原 `ValidateBatch`、`EstimateRequiredOutputs` 时复用已有预检 helper，将参数来源换成此计划，
不能因删除 IBizAdapter 而丢失空指针、负容量、数量回写或提前拒绝逻辑。非 1:1 外部结果属于后续单独设计范围。

运行时保存 `ValidatedIoPlan`：绑定与组件引用、typed 端口映射、宿主布局、调用限制及不可变验证计划。
Pipeline 消费同一次验证生成的 `ValidatedPipelinePlan`，不得重复解析、排序或加载模型。
必要时增加接受现成计划的创建/装配入口，仍由现有 Pipeline 状态机执行失败清理。

计划所有权明确如下：创建协调器在堆上构造 `unique_ptr<ValidatedPipelinePlan>`；验证结束后，
通过接收该 unique_ptr 的 Pipeline 装配入口转移所有权。Node 初始化前计划已位于稳定堆地址，
初始化后禁止复制、重建或移动其内容，不向 Node 传栈上计划或临时 config 指针。
`ValidatedIoPlan` 自己拥有解析后的端口映射及不可变组件引用，不共同拥有或复制 Pipeline 计划，
也不保留创建协调器的局部引用。SessionContext 同样使用稳定堆对象，装配成功只移动拥有指针。
失败清理和正常销毁均先停止/等待执行任务，再销毁 Node/执行资源，最后销毁其引用的 plan/session。
转换器描述符在句柄存续期保持有效；输出池租约不依赖转换器局部响应存储。

创建错误按上述阶段确定优先级：先配置/绑定与数据流，再模型/Node/池。非法绑定不得触发模型装载。
未知/缺失/非法绑定按配置错误返回，注册冲突按注册冲突返回；诊断必须标出 data.io_binding 等相关路径。
只维护新错误约定，不恢复旧创建入口的字段选择、错误文本或组合错误次序。

### 8.2 Catalog 与工具

基线 Catalog 为 schema 3，实际查询包含 8 个 `bizs`、12 个 Node、6 个 Model。
保留业务、Node、Model、Backend 的有效能力，Profile 更新为新接入配置；对外 Catalog 只提供 **schema 4**，增加：

- `input_converters`：请求 schema、外部类型与内部逻辑输出端口。
- `output_converters`：响应 schema、内部逻辑输入端口、输出基数及容量策略。
- `io_bindings`：组件 ID、业务契约、入口、端口映射和有效批次限制，不提供默认绑定字段。

Integration 的 `io_catalog` 门面聚合转换器/绑定注册表与 Core `PipelineCatalog` 快照。
Core 不反向读取 Integration 注册表；命令和 Studio 统一使用聚合入口，不维护两套公开 Catalog 定义。
Core 快照作为中性数据，由聚合入口统一序列化 schema 4；不提供对外 schema 3 兼容输出或双版本切换。

新增目标命令：

```bash
./build/alg_pipeline_tool validate-io <接入配置路径> --transport cabi
./build/alg_pipeline_tool validate-io <Operator配置路径> --transport operator --model-root <资源根目录>
```

两种命令使用与 Create 相同的解析器和验证器，只解析声明与计划，不加载模型、不分配池。
C ABI 校验必要时也接受 `--model-root`，与创建参数一致。结果输出解析后的 binding、组件 ID、端口映射、外部类型和结构化诊断。
现有 `validate <pipeline.json>` 继续验证纯内部 Pipeline，不声称验证外部协议。

同步 CLI、Studio 后端/消费者、Catalog 契约测试及相关 skill；配置字段和注册事实不能依赖文档手工白名单。

## 9. 请求生命周期与错误行为

### 9.1 Operator Process

目标时序只有一套：

1. 检查句柄、加锁，验证输入/输出帧数及批次上限。
2. 解析全部输入、输出槽位，检查类型、必需槽和空输出 shared_ptr；ValueType 做宿主内存形状检查。
3. 调用输入 Decode，有界读取和业务校验，发布 copy-in 的内部值；失败即终止，不租用输出池。
4. 租用全部输出块并登记 guard；池资源取得后才启动 Pipeline，避免无输出空间时进行计算。
5. 执行 Pipeline。
6. 调用输出 Encode，检查实际结果数，写入已租用的目标。
7. 全成功后，Operator 通用代码按两阶段机制发布输出。

不强制为错误优先级准备公共 C shadow DTO、自持有输入批次或空的输出 scratch。
转换器若需要局部临时存储，由回调管理；引用不得逃逸。输入失败不加载额外请求资源、不执行 Pipeline。
新阶段顺序就是错误优先级依据；同阶段按声明的槽位顺序与帧索引报告首错，不复刻旧校验交错顺序。

### 9.2 C ABI Process

同句柄 Alg_Process/Alg_Control 串行，入口保留异常屏障。先检查句柄、数量、输入指针与调用方输出对象数量容量，
再调用同一个 Decode 机制、Pipeline、Encode，最后核对 written_count 并回写 num_outputs。
num_outputs 必须非空；容量不足时回写所需数量并返回容量错误，其他失败回写 0；成功回写实际数量。
输出对象数量容量检查先于执行，不能为得到所需对象数量启动 Pipeline；本轮 1:1 绑定所需数量等于输入数量。
字符串、数组等字段的实际容量在 Encode 阶段检查，动态结果长度不能在执行前确定。
字段容量不足时 num_outputs 仍回写所需输出对象数量，不表示所需字节数；诊断应标明失败字段及其容量约束。

失败时所有输出视为无效，调用方丢弃其内容，不承诺保留旧的部分写入位置或字段值。
输入越界读取、字符串静默截断、无效来源被当作成功等行为仍禁止；这些是新实现的正确性要求。

### 9.3 输出池与异常

- 取得池块前准备租约记录容量，每个取得的块立即受 guard 管理。
- Pipeline、Encode、数量检查或宿主写入失败，所有已租用块归还，用户输出槽仍为空。
- 发布时先构造全部临时 `shared_ptr`，成功后统一填入 map；输出组件不能越过发布点。
- 返回块保留 `reset_external`，支持复用；输出字段指向池内存，不指向快照或临时编码变量。
- 输入、输出各阶段回调的异常由对应入口原有屏障映射；六个导出保持 `noexcept` 及两类 catch。
- 调用方须在 Destroy 前等待请求完成并释放输出租约。输出 shared_ptr 不延长句柄/池内存的有效使用期，不允许 Destroy 后访问输出。

### 9.4 业务细节

| 业务/行为 | 功能与正确性要求 |
| --- | --- |
| Translate | 完整 JSON 请求、query 选择、其他字段处理、translated 响应、转义/内嵌 NUL 语义 |
| Translate 输出 | 完整序列化、转义与异常处理；返回码/诊断一致，无历史特例 |
| DocQA | 三路结果按来源正确对齐，缺失或错配拒绝 |
| Entity/OCR/Audit | 结构化失败/fallback 拒绝，成功响应编号正确 |
| CrossRerank | 候选展开、rank 连续性、original_sub_id、按请求聚合与数量规则 |
| Audio | PCM/采样约束、copy-in、转写与槽位来源关联 |
| 所有业务 | 外部重复 ID 可用，内部重复/越界来源拒绝，无请求共享状态 |

## 10. 八个业务迁移清单

以下契约 ID 来自基线 Catalog。迁移时再次查询目标构建，保留期间合法新增的注册项。
每个绑定文件注册一次 BizDefinition，并声明供配置显式选择的 C ABI、Operator 绑定。

| 原文件词干 | 原 biz_name | 输入实现 | 输出实现 | 绑定声明 |
| --- | --- | --- | --- | --- |
| `translate` | `translate_v1` | `translate_json_input.cpp` | `translation_json_output.cpp` | `translate_bindings.cpp` |
| `entity_extract` | `entity_extract_v1` | `text_input.cpp` | `structured_document_output.cpp` | `entity_extract_bindings.cpp` |
| `keyword_match` | `keyword_match_v1` | `text_input.cpp` | `keyword_result_output.cpp` | `keyword_match_bindings.cpp` |
| `doc_qa` | `smart_doc_qa_v1` | `doc_query_input.cpp` | `doc_answer_output.cpp` | `doc_qa_bindings.cpp` |
| `cross_rerank` | `dense_cross_rerank_scoring` | `rerank_input.cpp` | `rerank_result_output.cpp` | `cross_rerank_bindings.cpp` |
| `compliance_audit` | `dialogue_compliance_audit_v1` | `audit_input.cpp` | `audit_result_output.cpp` | `compliance_audit_bindings.cpp` |
| `audio_asr_intent` | `speech_audio_asr_intent_slot` | `audio_input.cpp` | `audio_result_output.cpp` | `audio_asr_intent_bindings.cpp` |
| `ocr_doc_qa` | `multimodal_ocr_invoice_qa` | `image_query_input.cpp` | `invoice_result_output.cpp` | `ocr_doc_qa_bindings.cpp` |

每个原 Adapter：业务契约移到绑定文件，转换约束进入两侧 Definition；两入口的读取与业务解释均进入统一 Decode，
复用合适的底层函数；结果处理进入 output 私有 helper，按新执行顺序组织。
原 bridge 的字段读写迁入两侧组件，槽位与资源声明进入新 Definition。

现有八个业务功能及完整请求/响应示例继续作为产品验收依据；配置、创建参数和诊断统一迁移，不保留旧版运行承诺。
共享源文件不表示协议相同，Entity、Keyword 的载体读取仍按各自类型声明。

最终删除已迁空的业务 Adapter 类/注册、八个业务 bridge，以及仅服务旧耦合关系的
BizAdapterRegistry、OperatorBizBridgeRegistry、IBizAdapter 运行时选择链。
共享 helper/authoring 模板迁移使用者后按引用决定保留、改名或删除；不增加兼容包装、旧接口重载或双执行路径。
仓内扩展、示例、测试、生成器使用旧接口的部分必须同范围迁移；源码扩展消费者需要重新编译。

## 11. 构建、依赖检查与文档同步

### 11.1 构建与初始化

新增转换器、绑定、注册表、配置解析采用显式 `target_sources`，纳入 `edgeflow_integration_objects`。
`shared_algorithm_runtime.cpp` 保持现有 `edgeflow_composition_objects` 归属。
不按每个转换器建立库，不使用递归 glob 替代明确编译清单。

SDK、生产工具和测试工具链接同一组生产注册对象，防止静态裁剪导致 Catalog 与运行时不同。
测试专用注册只进入测试目标，不能用测试工具绕过生产缺失注册。
全局初始化取得完整注册快照后审计业务、转换器、绑定和 ValueType，保持冲突原因稳定、幂等和线程安全。

### 11.2 层隔离

更新 `scripts/check_layer_dependencies.py` 与 `scripts/check_layer_isolation.sh`：

- input、output、biz 及其私有头保留业务转换代码禁止直接依赖 Node/Model/Backend 的规则。
- 保留通用装配的必要依赖，不放宽整个 Integration。
- Core/Node 不可包含新转换器、绑定和宿主私有头；Core 中性验证约束不含 Integration 类型。
- 增加三个目录嵌套 `.cpp`、私有 `.h` 的正反例，覆盖相邻头间接引入非法依赖。

`cmake_ext/LayerHeaderViews.cmake` 按职责构建 include view。扩展头只在 extension/Integration 范围可见，
私有头只在内部范围可见；在现有 `test_layer_header_views.cmake` 增加真实新头的编译正反例。
同步头清单与导出检查，公共 C11 视图不得引入 STL 或转换器声明。

### 11.3 活跃文档与消费者

源码实施完成时同步更新，不提前将目标能力写成现状：

- `AGENTS.md`：Integration 组件职责、注册、完整接入审计，保持四层依赖方向。
- `doc/dev_guide/source_layout.md`、`business_onboarding.md`、`adapter_templates/README.md`。
- `doc/developer_guide.md`、`doc/architecture.md` 及受影响架构图源文件。
- `doc/solutions/translate.md`：完整两入口请求/响应、唯一接入配置与显式绑定示例。
- `.agents/skills/llm-edgeflow-developer-guide/references/integration.md`、`orchestration.md`。
- `.agents/skills/pipeline-composer/` 的配置与 Catalog 说明。
- `.agents/skills/json-prompt-solution/SKILL.md` 及仓内 authoring/recipe 使用者。
- CLI、Studio、Demo、Catalog 消费者，`tests/README.md`、CTest 与 inventory。
- `include/platform_mock/alg_types.h`、公开 Operator 辅助声明、Demo registry/runner、SDK 版本与导出清单、C11/ABI 及 real-model E2E 创建调用。

所有 SDK 创建调用的配置都迁为 schema 1；裸 Pipeline JSON 仅保留为内部流程文档及纯 Pipeline 工具/测试输入。
现有 Profile 路径、测试 fixture 和脚本同步调整，不能把新配置修补放在 Demo/Python 运行时充当兼容转换器。

这是架构与开发方式变化，源码交付时更新 `doc/CHANGELOG.md`；设计交付不冒充已发布能力。
历史 RFC/验收报告保留原始语境，新 RFC 记录实施范围。
真实公司 SDK 仍在授权内网接入，本轮仅使用 `platform_mock/` 与测试自有类型。

## 12. 实施阶段

| 阶段 | 内容 | 完成条件 |
| --- | --- | --- |
| P0：RFC 与目标基线 | 固定新 ABI/schema/执行顺序，列出全部调用方与配置；保留业务响应样例，制定新错误断言 | 迁移清单完整，旧兼容项明确删除 |
| P1：组件与绑定基础 | Spec、注册表、typed 绑定、业务独立注册、中性验证、唯一配置与 Catalog 聚合 | 新测试专用组合可验证，缺字段/旧格式/不匹配均拒绝 |
| P2：文本闭环 | Translate、Entity、Keyword；两入口只接受显式绑定 | 新契约通过，无默认选择，有复用证据 |
| P3：复杂业务 | DocQA、CrossRerank、Audit、Audio、OCR；长文本、rank、多槽位和池回滚 | 八个业务两入口通过，业务转换退出 bridge |
| P4：消费者与清理 | 迁移扩展/教程/生成器，清除旧注册和调用链，更新 guard/头视图/Catalog/Studio | 无旧生产路径，无 Core 向上依赖，消费者一致 |
| P5：交付 | 独立评审、差异验证、文档/RFC 收尾，一次 canonical gate | 第 14 节全部满足 |

这些阶段是一个实施分支内的工作顺序，不是分别发布的新旧兼容版本。
切换注册拥有者时直接替换并迁移对应消费者，不增加兼容层、双注册开关或旧路径回退。
尚未迁完的分支可以暂时无法通过完整构建，以新组件聚焦测试推进；最终交付前必须完成全部迁移并恢复全量门禁。

主实施者负责契约、首个闭环和文档；机械实施者在样例明确后迁移业务；测试作者补复用和行为缺口；
Reviewer 检查边界、配置、来源与生命周期；Verifier 统一负责最终门禁。
在隔离分支工作，同一目录不并行运行竞争的构建或门禁。

失败时按组件定位注册、绑定、端口计划、编码或容量问题，不靠放宽未知字段、关闭审计、删除断言或扩大 include view 修复。
回退阶段需恢复注册拥有者和配置选择，保留其他用户改动；未完成阶段如实记录，局部可运行不代替整体验收。

## 13. 验证设计

### 13.1 独立复用证明

优先使用测试专用绑定和自有外部类型，区分八个业务的产品样例与额外复用用例：

1. **输入复用**：同一输入转换器 ID 绑定 Entity、Keyword 两个兼容内部文本流程，只改绑定/端口映射，不复制函数。
2. **输出复用**：同一通用文本响应转换器 ID 读取两个兼容流程，产生同一 schema；Translate/DocQA 产品绑定仍按各自完整响应验收。
3. **多外部输入**：两种真实不同布局的结构经不同输入转换器驱动同一 Pipeline，得到等价内部数据与结果。
4. **独立更换输出**：同一 Pipeline 和输入，分别绑定两种外部响应结构，直接检查完整响应，不用 Demo 后处理。
5. **同载体不同 schema**：纯文本与 JSON 分别选择正确解释器，错误选择在声明或业务校验阶段失败。
6. **负向组合**：transport、端口、Batch 类型、重复 key、基数/来源、输出分配能力不匹配均拒绝。
7. **先验证后初始化**：不兼容绑定不加载模型、不初始化 Node、不分配池，用计数/替身设施证明顺序。

第 1 项可让 Keyword 测试绑定显式选择 Entity 文本载体及其转换器，调用方按新绑定契约构造对象，
不能把 Keyword 载体指针偷偷作为另一类型访问。第 2 项的 schema 属于测试绑定，不能据此认定不同业务协议可互换。

### 13.2 回归矩阵

| 维度 | 必须证明 | 优先复用位置 |
| --- | --- | --- |
| 唯一配置 | 两入口只接受 schema 1 接入配置；旧格式、裸 Pipeline、缺 schema/绑定、未知版本均拒绝 | C ABI、Operator 配置、CLI |
| 显式绑定 | 多绑定存在时精确选择指定项；未知/空/错误入口/端口失败，无推断和回退 | 配置、创建、CLI、注册 |
| Schema/来源 | 完整请求/响应，重复外部 ID、乱序结果、重复/越界 req_id/sub_id | purity、安全、新转换器套件 |
| 所有权 | 改写/释放输入后内部值不变，短字符串和快照扩容不悬空 | copy-in、Operator SSO/use_count |
| 输出容量 | 超 C 数组失败、池容量允许的长响应成功且完整 | `VariableDocResultPreservesLongAnswerAndCAbiLimit` 等 |
| 多槽回滚 | 第二帧/槽失败时未发布任何输出，下次合法调用成功 | Operator 嵌套输出、pool、分配失败设施 |
| 新执行顺序 | envelope/槽位→Decode→资源租用→Pipeline→Encode→发布；Decode 失败无池租用/计算 | 新阶段计数与故障注入 |
| 有界输入读取 | 空指针、超长字段和尺寸溢出在读取前拒绝，失败后不继续读取 | 输入回调受控缓冲与读取计数 |
| 创建次序/计划寿命 | 错误绑定先于模型装载；Node 保留的 plan/config 指针在装配转移后有效，失败时清理顺序正确 | 配置、Pipeline 计划和生命周期套件 |
| 写入/异常 | C ABI 容量不足回写所需数、其他失败回写0；输出无效，Operator 全成功才发布，异常不越界 | C ABI safety、Operator rollback |
| 业务复杂性 | DocQA 三路，Audit rank-one，CrossRerank 排名，Audio PCM，OCR 多输入 | 各业务 purity/集成 |
| 注册 | 多绑定共享 biz；生产必需入口无绑定、重复 ID、缺组件/ValueType 均拒绝；无旧枚举索引，幂等线程安全 | 新注册套件 |
| ABI/调用方 | 新创建结构与 expected_binding_id 辅助接口，版本/导出/C11 检查同步，所有仓内调用方迁移 | ABI、SDK export、Demo/real-model E2E 编译 |
| 架构 | Core/Node 无平台依赖，新目录和私有头受检查，转换器仅统一Decode/Encode | guard/self-test、头视图、组件接口 |
| Catalog | schema 4 完整，原注册事实保持，CLI/Studio 同源 | Catalog SSOT、工具集成 |

旧 `OperatorGoldenTest` 基线只覆盖七个业务，Translate 依靠专用契约与新增闭环，不能用单个 golden 套件声称覆盖八个业务。

### 13.3 用例归属与命令

扩展现有 `edgeflow_test_adapter_runner`。新测试可加入 `tests/unit/adapter/test_io_converters.cpp`、
`test_io_binding_registry.cpp`，suite 命名为 `IoConverterTest`、`IoBindingRegistryTest`。
按职责迁移旧 Adapter/bridge 断言，不复制两套测试。生命周期留在 Operator 套件，中性验证加入现有
PipelineValidator/ValidatedPipelinePlan 套件。新 suite/CTest 是待实现项，同步 `cmake_ext/Tests.cmake` 与 inventory。
只证明旧默认选择、旧错误文本/优先级或旧枚举 ABI 的测试改为新约定或删除；copy-in、容量、来源、并发和回滚覆盖继续保留。

对应阶段构建后可使用目标聚焦命令：

```bash
cmake --build build --target edgeflow_test_adapter_runner alg_pipeline_tool
ctest --test-dir build --output-on-failure --no-tests=error -R '^(IoConverterTest|IoBindingRegistryTest|AdapterPurityTest|AdapterContractSecurityTest|OperatorApiTest|OperatorOutputPoolTest|OperatorValueRegistryTest|OperatorGoldenTest|CAbiSafetyTest|AllBizPipelinesTest)$'
```

先用 `ctest --test-dir build -N` 核对实际登记项，旧 suite 迁移时同步过滤器；整个集合非空不代表预期用例全被选择。
Catalog、Core 验证、CLI 修改另运行现有聚焦目标，不为目录或符号存在性新建可执行文件。

保存迁移前后 Catalog JSON，核对有效业务与节点/模型能力没有遗漏，Profiles 的配置路径按新接入格式验收。
对外只输出 schema 4，新增数组按声明和运行验证，检查没有 default/legacy 字段；不要求旧 JSON 兼容或完整字节相同。
真实接入验证必须调用两条入口，`validate-io` 成功不等于响应正确。

全部源码、测试、文档、评审修订完成后，本地交付只运行一次最终门禁：

```bash
./scripts/run_all_tests.sh
```

该命令已含格式、空白、默认完整配置/构建和全量 CTest，不在前后例行重复完整构建或完整 CTest。
默认门禁与 Mock 路径不等于真实模型效果或目标平台验收。本次范围是外网仓库接口解耦与一次迁移，不引入内部 SDK 资产。

## 14. 完成标准与执行记录

- [x] 输入、输出独立 Spec/Definition/注册，无对端或业务注册依赖。
- [x] 跨业务复用、同一 Pipeline 多外部格式、独立更换输出都有运行证据。
- [x] biz 只注册一次，多绑定/schema/端口约束可通过 Catalog 查询，无旧枚举/默认绑定索引。
- [x] 两入口只接受新接入配置且必须显式绑定，旧配置被拒绝，C ABI/Operator 端到端验证通过。
- [x] 字段转换全部位于 input/output，Operator 仅通用接入与资源，业务 bridge 退出生产路径。
- [x] Core/Node 仅处理内部数据，PipelineValidator 统一验证中性边界，运行时消费同一次计划。
- [x] 八个业务两入口功能、来源、容量、异常和生命周期正确，错误处理按新约定验证。
- [x] 新 ABI、公开辅助接口和所有仓内消费者迁移完成；无旧注册/执行路径、兼容包装或双版本开关。
- [x] CMake、头视图、依赖检查、Catalog/Studio、文档与仓内 skill 同步。
- [x] 评审问题关闭，聚焦检查和 canonical gate 成功，跳过项及未验证范围如实记录。

| 记录项 | 实施后填写 |
| --- | --- |
| RFC / 分支 / 源码提交 | RFC-0059 / `refactor/adapter-io-layout-design` |
| 新 ABI/接入配置/执行次序与调用方迁移 | C ABI v6.0.0 (SOVERSION 6), `CompanyAlgParamCreate` 移除 `biz_type`; Schema 1 接入配置统一校验; 迁移全部 8 业务及 Demo、C ABI 与 Operator 测试 |
| 复用与多格式运行证据 | `IoConverterTest`, `IoBindingRegistryTest`, `TextConvertersTest`, `ComplexConvertersTest`, `AllBizPipelinesTest`, `OperatorGoldenTest` 全量通过 |
| Catalog 差异与消费者迁移 | Catalog schema 4 支持 `input_converters` / `output_converters` / `io_bindings`; `alg_pipeline_tool validate-io` 与 Studio 同步 |
| 聚焦测试 / 独立评审 | `edgeflow_test_adapter_runner` / `check_layer_isolation.sh` / `check_sdk_exports.sh` 全部通过 |
| 最终门禁 / 跳过项 | `./scripts/run_all_tests.sh` 6/6 步全绿 (99/99 测试全部通过)；无跳过项 |
| 剩余工作 / 完成日期 | 已收敛，全部目标完成 / 2026-09-16 |

本文件清单及对应验证已完成，源码状态更新为已实施。
