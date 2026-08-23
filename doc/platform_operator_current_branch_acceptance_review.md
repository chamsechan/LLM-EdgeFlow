# 当前分支相对 `main` 的验收复评

## 1. 复评结论

**结论：仍不通过，建议修复剩余 P1 后再次复验。**

本轮复评确认，上一次报告中的两个 P0 安全问题已经关闭：Control 已改为按命令解析唯一的强类型参数结构体；顺序重复 Destroy、Destroy 后 Process/Control 和随机非法句柄也不再直接解引用已释放对象。注册冲突 fail-closed、显式芯片白名单、RuntimeOptions 贯通、C ABI 入口异常屏障、输出分配 Hook 和 RerankRefineNode 单测也已落地。

最新提交能够完成全量构建，20/20 CTest 和六阶段回归全部通过，文档声明的正常生命周期与七个业务 happy path 基本可用。本轮没有发现新的 P0。

但仍存在 3 项 P1：部分模型覆盖时未覆盖模型的路径仍依赖当前工作目录；Deinit 在存在活跃句柄时静默遗失句柄及输出池；共享 Runtime 重构改变了既有纯 C ABI 的 Pipeline 构建失败返回码。这三项分别违反设计的路径确定性、生命周期所有权和“现有 C ABI 错误码保持不变”原则，因此当前仍不建议最终验收。

## 2. 评审范围与基线

| 项目 | 值 |
| --- | --- |
| 当前分支 | `docs/platform-operator-interface-design` |
| `main` | `a5fdf53` |
| 当前 HEAD | `09e9276` |
| 上次评审 HEAD | `3947dbc` |
| 本轮修复提交 | `0c8e95b`、`09e9276` |
| 设计基线 | `f857662a:doc/platform_operator_interface_design.md` |
| 比较范围 | `main...HEAD`，43 个文件，约新增 4400 行、删除 265 行 |

验收以用户指定提交 `f857662a` 中的设计文档为准，同时核对当前 README、当前设计文档及提交信息中的完成度声明。

## 3. 上轮问题复验结果

| 编号 | 复验结论 | 说明 |
| --- | --- | --- |
| P0-1 Control 猜测裸指针布局 | 已关闭 | 三个命令均有唯一参数结构体，按枚举确定性转换，并增加 null、长度、JSON 与范围检查 |
| P0-2 重复 Destroy UAF | 已关闭（按设计契约） | 活跃句柄表可安全拒绝顺序重复 Destroy、Destroy 后调用和随机地址；设计明确要求调用方在 Destroy 前停止并等待同句柄调用，因此并发 Destroy 不作为本轮强制能力 |
| P1-1 相对路径重复拼接 | 部分关闭 | `.conf`、Pipeline 路径和显式覆盖的模型路径已经绝对化；未覆盖模型仍保持相对路径，详见 P1-1 |
| P1-2 Registry fail-closed | 基本关闭 | GlobalInit 已审计 Business/Node/Engine/Platform I/O Registry，业务名唯一反查和 Create 期 I/O 绑定已落地 |
| P1-3 `depth_num` 所有权 | 基本关闭 | 已增加分配/释放 Hook 并按深度保存输出池；正常创建/销毁计数通过，但自动化测试尚未真正覆盖分配中途失败回滚 |
| P1-4 芯片和运行时元数据 | 已关闭 | 显式芯片白名单已接入，chip/batch/depth/device/business 已写入 RuntimeOptions |
| P1-5 C ABI 入口异常屏障 | 已关闭 | 六个 C 导出入口均恢复标准异常和未知异常双重 catch |
| P2-1 I/O alias 语义 | 已关闭 | canonical suffix 与 aliases 已显式分组 |
| P2-2 `.conf` 错误字段静默忽略 | 已关闭 | 已出现但类型错误、空字符串、未知 model_id 均确定性返回错误 |
| P2-3 平台错误类别丢失 | 已关闭 | Resolver 改为结构化返回 `-2/-5`，Registry 构建冲突保留 `-6` |
| P2-4 Rerank 未验证重排 | 已关闭 | 新增独立 `RerankRefineNodeTest`，覆盖可控分数重排、每请求 Top-K、空候选和缺失 Key |
| P2-5 测试/README 声明不准确 | 部分关闭 | 同句柄 Process/Control 已有并发用例，Rerank 已补单测；路径矩阵、真实 rollback 和活跃句柄 Deinit 仍未覆盖 |

## 4. 剩余高优先级问题

### P1-1：部分或零模型覆盖时，Pipeline 原始相对模型路径没有按 `.conf` 目录绝对化

设计第 5.3、5.4 节要求：模型相对路径统一以 `.conf` 所在目录为基准规范化；`model_paths` 允许只覆盖部分模型，未覆盖模型保留其语义，但仍必须得到确定的最终路径。

当前 Resolver 只对 `.conf` 中显式出现的 `model_path`/`model_paths` 做绝对化：

- `src/adapter/platform/company_conf_resolver.cpp:170-269`
- `src/adapter/platform_operator_adapter.cpp:163-171`

Create 随后把空字符串作为 `model_root_dir` 传给 Pipeline，并声称模型路径已“全量绝对规范化”。因此 Pipeline JSON 中未被覆盖的相对 `model_path` 会原样进入 Engine。

本轮从 `/private/tmp` 运行一个只覆盖 DocQA 两个模型中一个模型的 `.conf`，实际日志为：

```text
[MockNpuEmbeddingEngine] Loaded model from: /private/tmp/models/overridden_embed.bin
[MockNpuLlmEngine] Loaded LLM from: ./models/qwen_1.5b_npu.bin
```

第一个模型按 `.conf` 目录解析，第二个模型依赖进程 cwd，最终行为不确定。当前仓库正式 `.conf` 恰好全量覆盖了所有有模型的 Pipeline，因此六阶段回归没有暴露该问题。

建议在完成 `.conf` 覆盖后，再遍历内存 Pipeline JSON 中的全部模型：对仍为相对路径的 `model_path` 统一按 `.conf` 目录绝对化；或者传入唯一、明确的模型根目录，但不能同时出现两套相对路径基准。补充零覆盖、部分覆盖、全量覆盖及不同 cwd 的最终 Engine 路径断言。

### P1-2：Deinit 会静默清空活跃句柄登记，但不销毁句柄和输出池

`Platform_Deinit` 当前先调用 `PlatformHandleManager::ClearAll()`，后者只执行 `active_handles_.clear()`：

- `src/adapter/platform_operator_adapter.cpp:70-73`
- `src/adapter/platform_operator_adapter.cpp:414-424`

它没有删除 `PlatformHandle`、reset Runtime，也没有调用输出 deallocator。清空登记后，调用方再执行 Destroy 会被判定为非法句柄，已经没有公开路径可以回收该对象。

本轮定向探针结果：

```text
lifecycle create=0 alloc=2 deinit=0 dealloc_after_deinit=0 destroy_after_deinit=-1
```

这意味着 Deinit 返回成功，但两个预分配输出没有释放，句柄和 Runtime 同样泄漏。即使规范调用顺序应为 Destroy → Deinit，门面也不能在非法顺序下静默遗失资源。

建议二选一并形成稳定契约：

1. Deinit 检测到活跃句柄时返回 `-1`，不清空登记，允许调用方补做 Destroy；或
2. Deinit 原子摘取全部活跃句柄，按 Destroy 的同一释放逻辑逐个清理。

同时增加“活跃句柄 Deinit”“Deinit 后 Destroy”“带输出池 Deinit”和多句柄 Deinit 测试。

### P1-3：共享 Runtime 重构改变了既有纯 C ABI 的 Pipeline 构建失败码

设计第 1 节与第 13 节要求保留现有纯 C ABI V2，并在抽取共享 Runtime 后保持既有错误码和测试不变。

`main` 中，只要 `Pipeline::BuildFromConfigFile` 失败，`Alg_Create` 返回 `-3`。当前 `SharedAlgorithmRuntime::CreateFromConfigFile` 除 Registry 冲突外统一返回 `-2`：

- `src/adapter/shared_algorithm_runtime.cpp:116-132`
- `src/adapter/company_c_adapter.cpp:54-63`

本轮使用 business_name 合法但 node_type 未注册的 Pipeline 验证，当前返回：

```text
create_invalid_pipeline_ret=-2 handle_is_null=1
```

同一场景在 `main` 的实现路径返回 `-3`。这会改变依赖既有 C ABI 错误分类的宿主行为，而现有 C ABI 测试没有覆盖配置语义构建失败的精确返回码。

建议让 `CreateFromConfigFile` 保留旧 C ABI 映射，或由共享 Runtime 返回内部结构化错误类别，再由 C ABI 门面和平台门面分别映射：C ABI 保持兼容，平台 `.conf` 门面继续遵守新设计表。补充从 `main` 固化的错误码契约测试。

## 5. 中优先级问题与测试缺口

### P2-1：NaN 阈值可绕过 `[0, 1]` 校验

阈值校验只判断 `< 0.0f || > 1.0f`。IEEE NaN 与两边比较都为 false，因此会被序列化成 JSON `null` 并返回成功：

- `src/adapter/platform/platform_control_registry.cpp:109-142`

定向探针结果：

```text
[Pipeline] Control cmd received: 3, params: {"category":"VIP_SERVICE","threshold":null}
nan_threshold_ret=0
```

建议使用 `std::isfinite` 后再做范围校验，并增加 NaN、正负 Infinity 测试。

### P2-2：Platform I/O 注册只检查重复 BizType，没有验证设计声明的 Descriptor 不变量

设计要求 `biz_type + direction + suffix` 唯一，且 C 类型名与真实 Adapter 输入/输出结构一致。当前 `RegisterDescriptor` 只检查 BizType 是否重复，没有验证：

- canonical suffix/alias 是否为空或在同方向重复；
- group 的 direction 是否与 input_groups/output_groups 一致；
- `c_type_name`、`biz_name` 是否与 BusinessAdapter 契约一致；
- 第一阶段是否严格只有一个输入组和一个输出组。

涉及位置：

- `src/adapter/platform/platform_io_registry.cpp:15-29`
- `src/adapter/platform/platform_io_registry.cpp:159-274`

当前七个默认描述符本身正确，因此不是现有业务 happy path 故障；但公开注册 API 可以接收无法由 ExtractInputs/ExtractOutputs 正确表达的描述符，GlobalInit 仍会报告健康。建议在 RegisterDescriptor 中完整验证并记录冲突诊断。

### P2-3：新增测试名称仍高估了实际覆盖范围

- `DepthNumHookAndRollback` 只验证全部分配成功后的 Destroy 计数，没有让 allocator 中途返回 null/抛异常，也没有断言回滚数量。
- `HandleLifecycleAndUafPrevention` 覆盖顺序重复 Destroy 和销毁后调用，但不覆盖活跃句柄 Deinit。
- `.conf` 测试仍没有覆盖非法 JSON、缺字段、零/单/多模型、部分覆盖、未知 model_id 和不同 cwd；当前路径问题因此漏检。
- Control 测试覆盖有限区间，但没有 NaN/Infinity 和超长字符串。
- Platform I/O Registry 没有专门的描述符注册不变量测试。

建议让测试名与真实断言一致，并把上述场景作为最终验收门禁。

### P2-4：ccache 自动启用缺少显式开关，在受限环境中会使构建失败

`09e9276` 在发现 ccache 后无条件设置 C/C++ compiler launcher。本轮标准 `cmake --build build -j4` 因默认缓存目录不可写而失败：

```text
ccache: error: failed to create temporary file for
/Users/chenqichao/Library/Caches/ccache/tmp/...: Operation not permitted
```

设置 `CCACHE_DIR=/private/tmp/llm-edgeflow-review-ccache` 后完整构建通过。这不是平台业务实现缺陷，但会影响受限 CI/沙箱的开箱构建。建议提供 `LLM_EDGEFLOW_USE_CCACHE` 开关，并允许调用方显式关闭或配置缓存目录。

## 6. 已确认符合设计的实现

- 新增独立 C++ 平台公开头，纯 C 头未引入 STL/C++ 类型。
- 函数表六个函数指针完整、签名为 `noexcept`，入口具备双重异常屏障。
- C ABI 与平台门面共享 `SharedAlgorithmRuntime`，没有复制两套 Unpack → Execute → Pack 逻辑。
- `.conf` 与 Pipeline JSON 分层，内存修改 Pipeline JSON，没有生成临时配置文件。
- 强类型 Control 映射、字符串有界扫描、JSON object 校验和普通阈值边界已落地。
- Named I/O 使用最后一个点号解析后缀，alias group、必需槽位、额外槽位、空 shared_ptr 和 Batch 上限均有校验。
- 同一有效句柄的 Process/Control 使用同一 mutex 串行，不同句柄可独立执行。
- 顺序重复 Destroy、Destroy 后 Process/Control 和随机非法句柄会稳定返回 `-1`。
- BusinessAdapter 唯一反查、Create 期 I/O Descriptor 绑定和四类 Registry Init fail-closed 已落地。
- 显式芯片白名单及 chip/batch/depth/device/business RuntimeOptions 已贯通。
- `depth_num` 输出对象分配、正常 Destroy 释放和 Create 失败时已有实现级回滚逻辑。
- RerankRefineNode 遵守 `INode + REGISTER_NODE`、Blackboard 和 Engine 抽象，并新增可控分数单测。

## 7. 构建与测试记录

| 验证项 | 结果 | 说明 |
| --- | --- | --- |
| `cmake -S . -B build` | 通过 | FetchContent 依赖重新拉取并生成成功；存在上游 CMake deprecation/OpenMP 警告 |
| `cmake --build build -j4` | 条件通过 | 默认 ccache 目录在受限环境不可写；指定临时 `CCACHE_DIR` 后全量编译成功 |
| `ctest --test-dir build --output-on-failure` | 通过 | 20/20，约 13.4 秒 |
| `./scripts/run_all_tests.sh` | 通过 | 设置可写 `CCACHE_DIR` 后，LayerGuard、格式、构建、Tier 1～4、七业务 demo 和双 CLI 六阶段全部通过 |
| 部分模型覆盖、不同 cwd | 失败 | 未覆盖模型仍以 `./models/...` 进入 Engine |
| 活跃句柄 Deinit 探针 | 失败 | Deinit 返回 0，但输出释放计数为 0，后续 Destroy 返回 -1 |
| NaN Control 阈值探针 | 失败 | NaN 被序列化为 `null`，Control 返回 0 |
| C ABI 配置语义失败码 | 不兼容 | 当前返回 -2，`main` 对同类 Pipeline 构建失败返回 -3 |
| `git diff --check`、`git diff main --check` | 通过 | 当前工作区报告更新无空白错误 |

常规测试通过证明了正常主流程和已覆盖边界可用，但不能替代上述定向失败场景。

## 8. 建议整改顺序与复验门槛

1. 全量规范化 Pipeline 中未被 `.conf` 覆盖的相对模型路径，并增加不同 cwd 的最终 Engine 路径断言。
2. 明确 Deinit 对活跃句柄的拒绝或托管清理策略，确保句柄、Runtime 和输出池不泄漏。
3. 恢复纯 C ABI 的既有 Pipeline 构建失败码，平台门面使用独立映射时不要改变旧 ABI。
4. 拒绝 NaN/Infinity 阈值，补齐 Descriptor 注册不变量。
5. 把 allocator 中途失败回滚、活跃句柄 Deinit、配置矩阵和 Control 特殊浮点值纳入自动化测试。
6. 为 ccache 增加可关闭/可配置选项。
7. 重新执行全量构建、20+ CTest、六阶段回归与上述定向探针；P1 全部关闭后再作最终通过结论。
