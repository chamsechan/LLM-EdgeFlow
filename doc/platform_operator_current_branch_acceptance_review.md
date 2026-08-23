# 当前分支相对 `main` 的验收评审

## 1. 评审结论

**结论：不通过，建议整改后复验。**

当前分支已经实现了平台 Operator 兼容层的主要骨架，常规构建、19 组 CTest 和六阶段回归均通过；纯 C ABI 与 C++ 平台门面也确实复用了同一套 `SharedAlgorithmRuntime`。但是，设计中的异常安全、强类型 Control、句柄生命周期、注册冲突 fail-closed、配置路径规范化和 `depth_num` 所有权语义尚未完整落地。其中 Control 裸指针探测和重复 Destroy 存在进程崩溃/未定义行为风险，不满足平台接入的安全边界，不能按当前状态验收。

建议先修复本文 P0、P1 项，再重新执行完整测试矩阵和 sanitizer 验证。

## 2. 评审范围与基线

| 项目 | 值 |
| --- | --- |
| 当前分支 | `docs/platform-operator-interface-design` |
| `main` | `a5fdf53` |
| 当前 HEAD | `3947dbc` |
| 设计基线 | `f857662a:doc/platform_operator_interface_design.md` |
| 比较范围 | `main...HEAD`，共 40 个文件，约新增 3305 行、删除 275 行 |

设计文档在后续提交中被改成“已完成实现并全量回归测试”，且删除了原设计“本轮不直接实现接口/输出内存池/修改 demo”的非目标描述。本次验收严格以用户指定的 `f857662a` 版本为基线，同时对当前分支实际声称的“已完成交付”进行核查。

当前分支最后一个提交还增加了 `RerankRefineNode` 和 LLM + Rerank + QA 多模型 Pipeline。这部分不属于 `f857662a` 的平台 Operator 设计范围，但由于它位于当前分支相对 `main` 的差异中，本评审也检查了其架构和测试质量。

## 3. 阻断问题

### P0-1：Control 通过猜测 `void*` 内存布局解析参数，可能直接导致宿主进程崩溃

设计第 10 节明确要求：每个命令必须声明唯一参数结构体，通用门面不得猜测内存布局。

当前公开头只定义了 `ControlCommand`，没有为三个命令定义对应参数结构体。实现却依次把同一指针猜成 `const char*` 和 `const char**`，并执行首字节读取、二次指针解引用及无界 `strlen`：

- `src/adapter/platform/platform_control_registry.cpp:36-56`
- `include/platform/platform_operator_interface.h:20-27`

如果调用方按设计传入阈值、Prompt 或规则结构体，该代码可能把整数/浮点数字节解释成地址并解引用。`try-catch` 无法捕获 SIGSEGV，因此 `noexcept` 不能形成异常安全屏障。这也与 README 中“强类型动态控制映射”的交付声明不符（`README.md:130`）。

建议：

1. 在平台公开头中为每个 `ControlCommand` 定义唯一、明确的参数结构体。
2. 注册表按命令做确定性 `static_cast`，逐字段执行 null、长度和范围校验。
3. 字符串读取必须带上限；禁止探测任意裸内存、禁止对未知结构执行 `strlen`。
4. 增加三个命令的正常、null、非法范围、超长字符串、未知命令和异常注入测试。

### P0-2：重复 Destroy 测试本身触发 use-after-free，生命周期保护无效

`Platform_Destroy` 在第一次调用中将 `is_valid` 置为 false 后立即 `delete h`；第二次调用又把原地址转换成 `PlatformHandle*` 并读取 `h->is_valid`：

- `src/adapter/platform_operator_adapter.cpp:257-272`
- `tests/test_platform_operator.cpp:172-177`

第二次读取已经释放的对象属于 use-after-free。当前普通构建下测试偶然返回 `-1`，不代表实现安全；内存被复用后可能二次释放、崩溃或错误访问其他对象。同理，Destroy 后的 Process/Control 也会先解引用悬挂句柄。

建议：使用可校验的句柄注册表/控制块，在删除对象前从活动表原子摘除，并让公开入口先验证句柄是否仍登记；或者调整 API 为可清空调用方句柄的所有权模型。补充重复 Destroy、Destroy 后 Process/Control、非法句柄和并发销毁测试，并在 ASan 下通过。

## 4. 高优先级问题

### P1-1：相对路径没有按设计规范化，存在重复拼接模型目录的问题

`CompanyConfResolver` 在 `.conf` 为相对路径时，把模型覆盖写成相对的 `base_dir/model_path`；`Platform_Create` 随后又把 `conf_path.parent_path()` 作为 `model_root_dir` 传给 Pipeline：

- `src/adapter/platform/company_conf_resolver.cpp:26-37,145-180`
- `src/adapter/platform_operator_adapter.cpp:102-107`

从仓库根目录执行以下命令可以直接观察到错误路径：

```bash
./build/alg_demo --biz 1 --conf configs/pipeline_entity_extract.conf
```

实际加载日志为：

```text
configs/configs/models/qwen_0_6b_npu.bin
```

Mock Engine 不检查文件存在性，因此回归仍然通过；真实 Engine 或部署路径会受影响。设计要求 `.conf`、Pipeline 和模型相对路径均以 `.conf` 所在目录为基准规范化后写入内存 JSON。

建议：先把 `.conf` 路径转成 `absolute(...).lexically_normal()`/受控 canonical 路径，再生成绝对 Pipeline 和模型路径；已写入绝对模型路径时不要再次传入会参与拼接的 `model_root_dir`。增加“从仓库根、build 目录和其他 cwd 使用同一相对/绝对 `.conf`”的路径断言测试，直接检查 Engine 的最终加载路径。

### P1-2：注册冲突和业务唯一反查未按设计 fail-closed

设计要求 Node、Engine、BusinessAdapter、Platform I/O 注册冲突在 Init 阶段 fail-closed，并要求 `business_name` 唯一反查 Adapter。

当前实现存在以下缺口：

- `SharedAlgorithmRuntime::GlobalInit` 只检查 `BusinessAdapterRegistry`，未检查 `NodeFactory`、`EngineFactory` 和 Platform I/O Registry。
- `PlatformIoRegistry` 通过 `unordered_map::operator[]` 硬编码写入，没有注册 API、重复检测或冲突状态。
- `GetAdapterByPipelineName` 找到第一个匹配项就返回，无法识别两个 Adapter 的 `allowed_pipeline_names` 重叠（`include/adapter/business_adapter_registry.h:134-142`）。
- Create 阶段不校验业务是否存在 Platform I/O Descriptor；缺失描述符要到首次 Process 才返回 `-5`（`src/adapter/platform/platform_io_registry.cpp:116-123`）。
- Pipeline 的 Node/Engine 冲突诊断最终会被 `CreateFromPipelineJson` 统一映射成 `-3`，没有保持设计的 `-6`。

建议：统一全局注册健康检查；让业务反查返回“0/1/多匹配”的确定状态；Create 时绑定并保存 Adapter 与 I/O Descriptor；按诊断类别保持 `-5/-6` 错误码。

### P1-3：`depth_num` 和输出所有权只存值，没有实现设计语义

当前 `depth_num` 只做非零校验并保存到 `PlatformHandle`，后续没有任何读取、输出对象创建/销毁钩子或 opaque 输出内存上下文：

- `src/adapter/platform_operator_adapter.cpp:21-28,84-87,114-119`

这不等价于设计第 7 节所述“Create 阶段创建 `depth_num` 组输出对象，Destroy 阶段统一销毁”。如果公司内存 API 尚未提供，这可以作为明确的待集成项，但不能将当前版本声明为完整实现。至少应提供可注入的创建/销毁 hook 接口及失败回滚测试；若本阶段明确不实现，应同步修正文档、README 和验收口径。

### P1-4：芯片校验与运行时元数据贯通不完整

Create 只拒绝 `ChipType::kUnknown`，任何强转得到的其他枚举值都会被接受（`src/adapter/platform_operator_adapter.cpp:79-82`）。`ChipType`、平台最大 Batch 和 `depth_num` 仅保存在平台句柄，未写入 `SessionContext::RuntimeOptions`；RuntimeOptions 仍只有 device、业务和路径字段。

这不满足设计“拒绝未支持芯片类型”及“将 device_id、芯片类型和其他运行时元数据写入 SessionContext”的要求。建议使用显式支持表校验枚举，并补充 RuntimeOptions 字段与下游可观测测试。

### P1-5：纯 C ABI 的 `Alg_Init`/`Alg_DeInit` 失去入口级双重异常屏障

仓库最高规则要求六个 C 导出函数均在入口处使用 `noexcept` 和标准/未知异常双重 catch。重构后 `Alg_Init` 与 `Alg_DeInit` 没有入口级 try-catch：

- `src/adapter/company_c_adapter.cpp:17-24`
- `src/adapter/company_c_adapter.cpp:147-153`

虽然共享 Runtime 当前内部捕获异常，但外层日志输出等代码仍可能抛出，`noexcept` 会导致 `std::terminate`。应恢复六个导出函数一致的入口屏障，并保留 C ABI 回归。

## 5. 中优先级问题

### P2-1：I/O 描述符的 `required` 语义与提取逻辑不一致

每个业务同时登记两个输入和两个输出槽位，且全部标记 `required=true`，例如 `keyword_in` 与 `sentence_in`（`src/adapter/platform/platform_io_registry.cpp:14-23`）。提取逻辑却接受其中任意一个，并在同一样本出现第二个合法槽位时直接报“Duplicate”（`src/adapter/platform/platform_io_registry.cpp:137-198`）。`required` 字段实际上从未参与校验。

如果这些名称是别名，应在描述符中显式表达 alias group；如果它们是必需的独立槽位，则提取结果不能只返回单指针。当前元数据会误导注册校验和后续扩展。

### P2-2：`.conf` 的错误字段类型会被静默忽略

`model_path` 非字符串时被当作未提供；`model_paths` 非对象时也被忽略；映射中的非字符串 value 直接 `continue`（`src/adapter/platform/company_conf_resolver.cpp:145-175`）。这与设计中的严格参数错误和测试矩阵不符，拼写/类型错误可能静默退回 Pipeline 原路径。

建议对已出现但类型错误、空字符串及非法映射值统一返回 `-2`，诊断中包含 JSON 字段路径。

### P2-3：错误码映射不完全符合设计

`CompanyConfResolver` 只返回 bool，Platform Create 将所有解析失败统一映射为 `-2`（`src/adapter/platform_operator_adapter.cpp:92-97`）。其中未注册业务/业务绑定错误按设计应为 `-5`。输入输出 Batch 数量不一致当前返回 `-3`，而输出 Batch 不可用在错误码表中定义为 `-4`。

建议让 Resolver/Runtime 返回结构化错误类别，不要靠字符串推断。

### P2-4：新增 Rerank Pipeline 的测试没有验证“重排”效果

新增节点符合 `INode + REGISTER_NODE`、黑板通信和 Engine 抽象要求，也有对应 CTest 目标。但 `tests/test_doc_qa_rerank.cpp` 只验证最终 intent、answer 非空和 chunk_count，未断言 Rerank 前后顺序、Top-K、分数对应关系、空候选、非法 `top_k` 或 Engine 输出数量异常。

建议增加 `RerankRefineNode` 的独立 Google Test，使用可控分数的测试 Engine，直接验证 `(req_id, sub_id)` 对齐和每请求 Top-K 结果。

### P2-5：README 的完成度和测试覆盖声明不准确

README 声称 Control 已做“参数结构体强类型校验”，并声称平台测试覆盖多模型覆盖和同句柄并发互斥（`README.md:130-131`）。实际测试：

- Control 只传入 JSON C 字符串。
- 没有单/多模型覆盖及未知 model_id 负向断言。
- 名为 `MultiHandleConcurrencyAndSerialization` 的测试只让每个线程使用不同句柄，没有同句柄 Process/Control 竞争（`tests/test_platform_operator.cpp:437-484`）。
- 重复 Destroy 用例本身是 UAF。

应在修复实现和补齐测试后更新交付声明。

## 6. 已符合设计的部分

以下实现方向正确，建议保留：

- 新增独立 C++ 平台公开头，未把 STL/C++ 类型放入纯 C 头。
- `OperatorFunc` 六个函数指针均非空，命名和签名基本符合设计。
- 从 C ABI 中提取 `SharedAlgorithmRuntime`，C 与平台门面共享 `ValidateBatch -> Unpack -> Pipeline::Execute -> Pack`，没有复制两套执行逻辑。
- 平台配置在内存中修改 Pipeline JSON，没有创建临时 JSON 文件。
- Named I/O 使用最后一个点号解析后缀，并通过 `shared_ptr<void>::get()` 借用底层指针，没有复制业务 C 结构体。
- Process 校验空 Batch、输入输出数量和平台 Batch 上限；业务 Adapter 仍负责自身最大 Batch 和 DTO 字段校验。
- 同一有效句柄上的 Process/Control 使用同一 mutex 串行化；不同句柄可独立执行。
- 新增平台 demo、平台测试目标、Rerank Pipeline 测试目标并注册到 CTest。
- 新增 `RerankRefineNode` 位于 `src/common_nodes/`，使用 Blackboard 和 `IRerankEngine`，未产生反向层依赖。
- README 已记录平台兼容层这一主要架构变化。

## 7. 测试与复现记录

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| `cmake -S . -B build` | 通过 | 首次配置需下载 FetchContent 依赖 |
| `cmake --build build -j4` | 通过 | 有若干既有 unused-parameter 警告 |
| `ctest --test-dir build --output-on-failure` | 通过 | 19/19，通过时间约 11.9 秒 |
| `./scripts/run_all_tests.sh` | 通过 | 六阶段脚本全部通过；脚本会执行格式化和重新构建 |
| 从仓库根运行相对 `.conf` | 发现问题 | 模型加载日志出现 `configs/configs/models/...` |
| ASan/UBSan 补充验证 | 未形成有效结论 | sanitizer 构建成功，但 AppleClang ASan 目标在测试启动阶段因 runtime 初始化检查中止，未进入测试体 |

常规测试全部通过只能说明 happy path 和部分输入校验可工作，不能消除本文发现的裸指针解析、UAF 和缺失语义。

## 8. 设计测试矩阵缺口

对照设计第 14 节，至少还缺少：

- `.conf` 非法 JSON、缺少字段、错误字段类型、空路径、不同 cwd 路径规范化。
- 零模型忽略单路径、单模型覆盖、多模型映射、单路径配多模型失败、未知 model_id、部分覆盖。
- business_name 零匹配和多匹配；Business/Node/Engine/I/O 冲突的 Init fail-closed。
- 空后缀、首点号、尾点号、重复后缀、方向错误、额外槽位、输出 null。
- 三个 Control 命令的强类型正常/异常路径及未知命令。
- Process 不替换输出地址，Create/Destroy 的 `depth_num` 输出对象数量和失败回滚。
- 同句柄 Process/Control 串行化，不同句柄并发，以及受控 Destroy 生命周期。
- 每个公开入口的标准异常/未知异常注入验证。
- sanitizer 下的重复 Destroy、Destroy 后调用和 Control 非法结构测试。

## 9. 建议整改顺序与复验门槛

1. 先修复 Control 强类型契约和句柄 UAF；这两项属于安全阻断。
2. 修复路径绝对化/单次解析，并增加最终 Engine 路径断言。
3. 补齐统一 Registry 健康检查、唯一业务反查、I/O Descriptor Create 期绑定和错误码映射。
4. 明确并实现 `depth_num` hook，或将其正式标记为待公司接口落地、撤销“已完整实现”的声明。
5. 贯通 ChipType/平台 Batch/depth 元数据并恢复 C ABI 六入口异常屏障。
6. 补齐设计测试矩阵与 Rerank 节点单测，在 ASan/UBSan 可正常运行的环境中执行。
7. 最后重新运行格式化、19+ CTest、六阶段回归和平台 demo；P0/P1 全部关闭后再验收。

