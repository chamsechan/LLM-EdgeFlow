# LLM-EdgeFlow V2 架构整改验收记录

> 验收日期：2026-08-19  
> 验收分支：`docs/architecture-review-ai-friendly`  
> 验收范围：纯 C ABI、Business Adapter、四层依赖、RuntimeOptions、架构门禁、文档一致性及回归验证  
> 总体结论：**暂不通过；V2 架构方向合理，当前实现应定义为 Phase 1 部分完成。**

## 1. 总体判断

`doc/architecture_v2.puml` 描述的目标架构仍然合理：保留四层平台内核，在其上建设 Business Manifest、Schema、Catalog、脚手架、静态校验和测试工具，可以同时降低普通业务开发者和 AI 的接入难度。

本轮实现已经完成以下有价值的基础工作：

- 将公共批处理入口调整为可由 C11 调用的指针数组接口。
- 抽取 `IBusinessAdapter` 和 `BusinessAdapterRegistry`，消除中心 Adapter 中的大型业务分支。
- Layer 3 业务后处理节点不再直接依赖 `company_alg_interface.h`，改为领域 DTO。
- 新增 C11 ABI 测试和 Layer Guard 脚本。
- 新增 `RuntimeOptions`，开始传递模型根目录和设备参数。

但当前仍存在 ABI 兼容性、异常安全、批处理输出契约等发布阻断问题，尚不能将 ARCH-001、ARCH-003 和 ARCH-010 判定为完整整改。

## 2. 发布阻断项

### ACC-001：ABI 已发生破坏，但动态库版本仍为 SOVERSION 1

**级别：P0**  
**状态：待整改**

`include/company_alg_interface.h:182` 将原有三参数 `Alg_Process` 替换为五参数纯 C 接口。当前 C++ inline wrapper 只能提供重新编译后的源码兼容，不能提供已有二进制的兼容。

`CMakeLists.txt:82-83` 仍声明：

```cmake
VERSION 1.0.0
SOVERSION 1
```

如果已有下游程序使用旧头文件完成链接，它会继续按三参数调用同名 `Alg_Process` 符号，而新动态库按五参数解释调用现场，行为未定义并可能导致宿主崩溃。

**整改建议**

二选一：

1. 保留旧 `Alg_Process` ABI 符号，新增具备独立符号名的 `Alg_ProcessV2`，旧接口作为迁移入口。
2. 明确进行 ABI 大版本升级，将动态库升级为 `VERSION 2.x / SOVERSION 2`，同时停止声明二进制向后兼容。

若项目确认从未对外交付旧动态库，可以降低风险等级，但仍需修正 ABI 版本和兼容性说明。

### ACC-002：六个公共 C 接口尚未形成完整的异常防火墙

**级别：P1**  
**状态：待整改**

`include/company_alg_interface.h:165-198` 的六个接口没有声明与 C/C++ 条件编译兼容的 `noexcept`。实现中：

- `src/adapter/company_c_adapter.cpp:23` 的 `Alg_Init` 没有异常捕获。
- `src/adapter/company_c_adapter.cpp:173` 的 `Alg_DeInit` 没有异常捕获。
- 其他接口没有保证整个函数体都位于统一的异常屏障内。

此外，`include/adapter/business_adapter_registry.h:48` 的自动注册宏会在 `Alg_Init` 之前执行 `std::make_shared`、容器写入和日志输出。端到端运行日志也表明 Adapter 注册发生在 `Alg_Init` 之前。若静态初始化期间抛出异常，异常不会经过六个 C 接口，进程可能直接 `std::terminate`。

**整改建议**

- 为 C++ 编译定义可移除的 `COMPANY_ALG_NOEXCEPT` 宏，并让声明、定义保持一致。
- 六个接口的全部实现逻辑均置于统一 `try/catch` 中，稳定映射为错误码。
- 将可能分配内存或失败的注册初始化移入显式初始化阶段，或采用不抛异常的静态描述符/工厂表。
- 增加异常注入测试，验证 Registry、Adapter、Pipeline、Node 和 Engine 抛出的异常都不会越过 C ABI。

### ACC-003：所有业务 Adapter 的输出容量契约实现错误

**级别：P1**  
**状态：待整改**

公共头文件约定 `num_outputs` 输入为 `outputs` 容量，输出为实际填充数量。但七个 Adapter 都执行了以下等价逻辑：

```cpp
for (int i = 0; i < count && i < out_limit; ++i) {
  // 最多填充 out_limit 个结果
}
*num_outputs = count;  // 报告全部结果数
return 0;
```

代表位置：

- `src/adapter/adapters/keyword_match_adapter.cpp:46-59`
- `src/adapter/adapters/entity_extract_adapter.cpp:46-57`
- `src/adapter/adapters/doc_qa_adapter.cpp:48-66`
- `src/adapter/adapters/compliance_audit_adapter.cpp:51-76`
- `src/adapter/adapters/ocr_doc_qa_adapter.cpp:48-64`
- `src/adapter/adapters/audio_asr_intent_adapter.cpp:55-71`
- `src/adapter/adapters/cross_rerank_adapter.cpp:54-68`

容量小于结果数时，只填充部分输出，却报告全部结果都已填充。若某个 `outputs[i]` 是空指针，当前实现还会静默跳过并返回成功。

**整改建议**

- 明确选择并记录统一契约：
  - 容量不足时返回专用错误码，同时通过 `num_outputs` 返回所需容量；或
  - 允许部分填充，但只返回实际成功填充的数量。
- 对批量数组中的每个空指针进行确定性校验，不允许静默成功。
- 抽取公共 `ValidateBatch`/Pack 辅助逻辑，避免七个 Adapter 复制同一缺陷。
- 增加容量为 0、容量不足、空槽位、结果为空和超大批次测试。

## 3. 架构完整性问题

### ACC-004：`model_root_dir` 和 `device_id` 尚未真正贯通到 Engine

**级别：P1**  
**状态：待整改**

`src/core/pipeline.cpp:187` 使用字符串方式拼接 `root + model_path`。当前配置中的模型路径普遍为 `./models/foo`，调用方传入的根目录也为 `./models`，因此候选路径会成为：

```text
./models/./models/foo
```

候选文件不存在时实现会静默回退到原始路径。实际端到端日志仍然显示 Engine 加载 `./models/foo`，说明当前 `model_root_dir` 没有改变模型解析结果。

`src/core/pipeline.cpp:193` 仅在 `device_id != 0` 时注入配置，但设备 0 本身是合法目标设备；目前各 Engine 也没有读取该字段。因此 README 中“运行时环境与设备参数已贯通”的描述不准确。

**整改建议**

- 使用 `std::filesystem::path` 并明确模型配置路径语义：相对配置文件、相对模型根目录或绝对路径。
- 明确 `RuntimeOptions`、Pipeline 配置和 Engine 默认值的优先级。
- 无条件传播合法的设备 ID，使用独立的“未指定”值而不是复用 0。
- 增加非默认临时模型根目录、设备 0、设备 1、配置覆盖和文件不存在测试。
- 让 Engine 提供可观察的已解析设备/路径信息，避免测试只能通过日志推断。

### ACC-005：Adapter Registry 尚不能防止多团队注册冲突

**级别：P1**  
**状态：待整改**

`include/adapter/business_adapter_registry.h:25` 使用：

```cpp
adapters_[biz_type] = adapter;
```

重复业务 ID 会静默覆盖旧 Adapter，注册顺序变化可能导致请求被路由到错误实现。当前 `IBusinessAdapter` 也只有 `BizType`、`BizName`、`Unpack` 和 `Pack`，尚未具备 V2 架构图中规划的 ABI 版本、Descriptor 和 `ValidateBatch`。

**整改建议**

- 重复 `biz_type`、业务名称和 ABI 版本必须注册失败，并输出确定性诊断。
- 引入机器可读 Adapter Descriptor，至少描述业务 ID、ABI 版本、输入输出类型和批处理限制。
- 增加重复注册、缺失注册、未知业务 ID 和注册顺序测试。
- 后续由 Business Manifest 或统一 ID 分配机制管理业务 ID，减少并行开发冲突。

### ACC-006：Layer Guard 尚不是完整的 CI 强门禁

**级别：P2**  
**状态：待整改**

README 将 `scripts/check_layer_isolation.sh` 描述为 CI 强门禁，但当前脚本没有接入 CTest、`scripts/run_all_tests.sh` 或仓库 CI。

脚本目前仅检查 Layer 3 是否包含 Layer 1 头文件，以及公共 C 头文件的部分 include。`scripts/check_layer_isolation.sh:25` 还全局排除了 `<vector>`，如果它被误放到纯 C 区域，Layer Guard 自身仍可能放行。

`src/adapter/adapters/audio_asr_intent_adapter.cpp:7` 仍由 Layer 1 直接包含 Layer 4 的 `engine/engine_interface.h`，并在黑板中保存 `IAudioAsrEngine::AudioPcmData`。这绕过了 V2 图中的内部业务输入 DTO，也没有被当前 Layer Guard 检测。

**整改建议**

- 将 Layer Guard 注册为 CTest，并接入 `run_all_tests.sh` 和 CI。
- 检查完整的目录依赖矩阵，而不只检查 Layer 3 到 Layer 1 的单向依赖。
- 使用严格 C11 编译测试验证纯 C 头文件，不通过白名单方式排除 `<vector>`。
- 为音频业务增加 `AudioPcmDto`，让 Layer 1 只依赖业务契约和 Layer 2 上下文，由 Layer 3 转换为 Engine 能力类型。

### ACC-007：架构评审、目标图和 README 的状态描述不一致

**级别：P2**  
**状态：待整改**

`doc/architecture_review.md` 仍把以下内容描述为当前事实：

- 公共 ABI 使用 `std::vector`。
- Layer 3 直接依赖 Layer 1 结构体。
- `company_c_adapter.cpp` 维护中心业务分支。

上述实现已经发生变化，而 README 又把 ARCH-001、ARCH-002、ARCH-003 和 ARCH-010 描述为已完成。对于 AI 友好的框架，这种事实来源冲突会直接导致错误代码生成和错误的修改范围判断。

**整改建议**

- 为问题增加“基线状态、当前状态、目标状态、验收状态”字段。
- 已完成基础重构但未满足全部验收条件的项目标记为“部分整改”，不要直接标记为已完成。
- 明确 `architecture_v2.puml` 是目标架构，而不是当前实现图。
- 让 README 只引用统一状态表，避免重复维护架构完成度。

### ACC-008：代码差异检查存在格式问题

**级别：P2**  
**状态：待整改**

`git diff --check main...HEAD` 报告：

```text
CMakeLists.txt:152: new blank line at EOF.
```

应在交付前修复，并把 `git diff --check` 加入本地和 CI 门禁。

## 4. 已通过的验证

本轮以只读方式完成以下验证：

| 验证项 | 结果 |
|---|---|
| CMake Release 配置 | 通过 |
| 完整工程编译 | 通过 |
| CTest | 12/12 通过 |
| `cc -std=c11 -pedantic-errors` 公共头文件编译 | 通过 |
| `scripts/check_layer_isolation.sh` 单独执行 | 通过 |
| 七个业务 `alg_demo` 端到端运行 | 通过 |
| Python `./show` 全部 Pipeline 配置 | 通过 |
| C++ `./build/alg_show` 全部 Pipeline 配置 | 通过 |
| 工作树干净性 | 通过 |
| `git diff --check` | 未通过，存在一个 EOF 空行问题 |

没有直接执行 `scripts/run_all_tests.sh`，因为其第一步会调用 `clang-format -i` 修改整个源码树，不符合本轮只读验收边界；其中构建、测试、Demo 和工具运行部分已分别执行。

## 5. 建议整改与复验顺序

1. 先确定旧 ABI 是否已经对外交付，并完成 ABI 符号和 SOVERSION 决策。
2. 实现六接口完整 `noexcept`/异常屏障，消除可能抛异常的预初始化流程。
3. 统一七个 Adapter 的输出容量与空指针契约，并补齐边界测试。
4. 修复模型路径和设备参数传播，增加可观察的 RuntimeOptions 测试。
5. 为 Registry 增加冲突检测、Descriptor 和确定性错误诊断。
6. 把 Layer Guard 接入 CTest、全量测试脚本和 CI，并扩展为完整依赖矩阵。
7. 同步架构评审、README 和目标图的状态定义，修复 `git diff --check`。

完成前三项后可进行第一次发布阻断复验；完成全部项目后，再判断 Phase 1 是否可以正式验收。
