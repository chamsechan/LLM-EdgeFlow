# LLM-EdgeFlow 架构与实现审计报告

## 1. 审计结论

当前四层职责设计总体合理，运行时抽象也已有较好基础；但实现上更接近“按目录划分的模块化单体”，还没有形成由编译系统强制保证的分层架构。

不建议按当前状态投入生产。至少有 3 项发布阻断问题、4 项重要架构债务应先整改。

| 维度 | 评价 |
| --- | --- |
| 四层职责设计 | 基本合理 |
| 运行时契约 | 大部分清晰，但存在 Validator 假阴性 |
| 编译期层间隔离 | 较弱，全部实现编入同一个目标 |
| 层内内聚 | 中等，Core 和 Operator 存在明显“大文件中心” |
| 对外 ABI | 不合格，版本矛盾且符号严重泄漏 |
| 当前生产就绪度 | 不建议发布 |

审计基于 `main` 分支提交 `44f9b5f` 附近的工作区。除本报告外未修改项目源码。

## 2. 发布阻断问题

### P0-1：PipelineValidator 会接受运行时必然失败的 Pipeline

业务入口和节点输出分别记录：

- 入口写入 `ingress`：[`src/core/pipeline_validator.cpp`](../src/core/pipeline_validator.cpp#L708)
- 重复输出只检查节点 `producers`：[`src/core/pipeline_validator.cpp`](../src/core/pipeline_validator.cpp#L915)

因此节点可以把输出绑定到已经由 Adapter 发布的业务入口键。Validator 返回成功，但 Blackboard 是写一次语义：

- `AlgContext::Publish` 使用 `emplace` 拒绝重复键：[`include/core/alg_context.h`](../include/core/alg_context.h#L42)
- `BoundOutput::Set` 遇到重复键直接抛异常：[`include/nodes/node_support.h`](../include/nodes/node_support.h#L100)

动态复现方式是在 `pipeline_doc_qa.json` 中加入一个把输出重新绑定到业务入口 `raw_docs` 的 `TextChunkNode`，再通过 stdin 执行校验。重建后的 `alg_pipeline_tool` 仍返回：

```json
{
  "diagnostics": [],
  "ok": true
}
```

这构成 Validator 与运行时语义之间的确定性偏差。代码中的“last-writer semantics”注释也与真实的 write-once 行为冲突。

建议：

1. 输出端口校验同时检查节点生产者和业务入口键。
2. 为入口覆盖添加 Validator 和 CLI 负例测试。
3. 统一注释、诊断码和运行时写一次语义。

### P0-2：公共产品版本存在多个事实源且已经漂移

- CMake 产品版本是 `10.0.0`：[`CMakeLists.txt`](../CMakeLists.txt#L2)
- README 当前里程碑是 `10.0.0`：[`README.md`](../README.md#L120)
- 架构文档声明产品版本为 `10.0.0`：[`doc/architecture.md`](architecture.md#L147)
- 公共 C 头仍声明 `8.0.0`：[`include/company_alg_interface.h`](../include/company_alg_interface.h#L7)

ABI major 和 `SOVERSION` 仍然都是 5，但客户端编译得到的产品版本与实际生成的 `libcompany_alg_sdk.so.10.0.0` 不一致。现有文档和 ABI 门禁均未检测这一漂移。

建议：

1. 由 CMake 单一生成版本头，删除人工维护的重复产品版本。
2. 增加 CMake、公共头、SONAME 和 Changelog 的一致性门禁。
3. 明确产品版本与 ABI 版本各自的兼容策略。

### P0-3：共享库缺少有效的 ABI 可见性控制

重建后的 `libcompany_alg_sdk.so.10.0.0` 指标如下：

| 指标 | 数值 |
| --- | ---: |
| 文件大小 | 8,990,344 bytes |
| 动态定义符号 | 8,401 |
| `Alg_*` 符号 | 6 |
| 名称匹配 `llm_edgeflow` | 1,186 |
| 名称匹配 `llama`/`ggml` | 5,331 |

实际导出包含 NodeFactory、BackendRegistry、具体 Node，以及大量 ggml/llama 实现，而公共头将标准 ABI 描述为 6 个纯 C 接口：[`include/company_alg_interface.h`](../include/company_alg_interface.h#L189)。

根源是所有实现和静态第三方库都链接进单个 SHARED target，且没有默认隐藏可见性或链接器版本脚本：[`CMakeLists.txt`](../CMakeLists.txt#L78)。这会扩大非预期 ABI、符号冲突、动态链接和兼容性风险。

建议：

1. 默认设置 hidden visibility。
2. 使用统一的 `COMPANY_ALG_API` 宏显式导出获准的 C、日志和 Operator API。
3. Linux 增加 linker version script。
4. 门禁中使用 `nm -D` 校验导出符号 allowlist。

## 3. 重要架构问题

### P1-1：层级只有目录边界，没有构建边界

Core、Nodes、Engine、Adapter 的 CMake 都只是向同一目标追加源文件：

```cmake
target_sources(alg_sdk PRIVATE ...)
```

示例：

- [`src/core/CMakeLists.txt`](../src/core/CMakeLists.txt#L1)
- [`src/common_nodes/CMakeLists.txt`](../src/common_nodes/CMakeLists.txt#L1)
- [`src/engine/CMakeLists.txt`](../src/engine/CMakeLists.txt#L1)
- [`src/adapter/CMakeLists.txt`](../src/adapter/CMakeLists.txt#L1)

由此产生的影响：

- CMake 无法阻止非法层间依赖。
- 任一公共内部头变化容易触发大范围重编译。
- 无法单独测试、替换或裁剪某一层。
- 最终共享库被迫包含全部 Node、Model 和 Backend。

现有 LayerGuard 只检查少数路径和正则。例如它只禁止 `src/adapter/adapters/` 引入 Engine：[`scripts/check_layer_isolation.sh`](../scripts/check_layer_isolation.sh#L65)。但 Layer 1 的共享运行时组合实现直接依赖 Engine Registry：[`src/adapter/shared_algorithm_runtime.cpp`](../src/adapter/shared_algorithm_runtime.cpp#L11)。

Layer 3 的 Node 支撑头又需要 Layer 2 的 AlgContext、NodeBase、SessionContext 和完整 Validator 定义：[`include/nodes/node_support.h`](../include/nodes/node_support.h#L12)。这说明当前层级图混合了运行时调用方向与编译期依赖方向。

建议明确两个当前缺失的架构角色：

1. 位于各层下方的轻量 `contracts/SPI` 层，承载 INode、端口定义、AlgContext 接口和推理协议。
2. 位于各层上方的 Composition Root，负责连接 Registry、Pipeline、Node、Backend 和 Adapter。

随后为各层建立独立 CMake targets；如需保留静态注册，可以采用 OBJECT libraries 汇入最终 SDK。

### P1-2：Layer 2 的公共契约成为业务键和类型的集中仓库

[`include/core/common_contracts.h`](../include/core/common_contracts.h#L202) 同时包含：

- 通用 DTO 和类型 traits
- 26 个全局 BlackboardKey
- `structured_verdicts`
- `matched_policy`
- `extracted_invoice_json`
- `intent_slots` 等业务语义

该头被约 40 个源码和测试文件直接包含。新增业务容易迫使修改 Layer 2，并重编译无关 Node，削弱了“业务行为由 Adapter 和 Pipeline JSON 组合”的原则。

建议拆为：

- 中立 capability payload 和 type traits
- 节点端口契约
- 每个业务 Adapter 自己的 ingress/egress keys

### P1-3：SessionContext 的资源接口丢失类型安全

[`include/core/session_context.h`](../include/core/session_context.h#L215) 使用字符串键和 `shared_ptr<void>`，读取时直接执行 `static_pointer_cast<T>`。同一个键以错误类型读取不会被检测，可能产生未定义行为。

建议引入 `SessionResourceKey<T>`，或在资源条目中保存 `std::type_index` 并在读取时校验，使 Session 资源与 Blackboard 的强类型设计保持一致。

### P1-4：Catalog 返回锁外可能失效的引用和指针

`Nodes()` 和 `Bizs()` 在释放互斥锁后返回内部 vector 引用；`FindNode()` 和 `FindBiz()` 返回 vector 元素指针：

- [`src/core/pipeline_catalog.cpp`](../src/core/pipeline_catalog.cpp#L291)
- [`src/core/pipeline_catalog.cpp`](../src/core/pipeline_catalog.cpp#L309)

后续注册、扩容和排序可能使这些引用或指针失效。当前静态注册时序大多掩盖了问题，但不适合未来动态扩展或并发注册。

建议在 `Alg_Init` 后冻结 Catalog，或者让查询接口返回值对象、不可变快照或具有稳定生命周期的共享对象。

## 4. 层内内聚与代码臃肿

### 4.1 Core 层

几个主要热点：

| 文件 | 行数 | 当前职责 |
| --- | ---: | --- |
| `pipeline_validator.cpp` | 998 | 配置归一化、模型兼容、DAG、端口闭包、并发规划 |
| `pipeline.cpp` | 773 | 构建、物化、执行、控制和部分 JSON Schema 校验 |
| `pipeline_config.cpp` | 562 | Pipeline 文档解析和配置处理 |

`PipelineValidator` 作为唯一校验入口的原则应当保留，但可以将内部规则拆为私有实现单元，例如拓扑规划、端口流校验、模型/Backend 兼容性和配置归一化。不要引入第二套公共 Validator。

### 4.2 Adapter/Operator 层

`src/adapter/operator/operator_value_type_registry.cpp` 达到 1,188 行，集中硬编码多种公共和业务值类型。新增业务 DTO 往往需要修改这个中心文件，存在开闭原则和编译扩散问题。

建议保留统一 Registry 查询接口，把每类描述符和注册动作拆至相邻业务或类型文件中，通过自注册或显式批量注册组合。

`include/adapter/templates/` 下的模板目前主要被测试和文档引用。如果它们不是正式对外 authoring SDK，应迁移至 `examples/` 或 `dev_support/`，减少正式 include 表面的歧义。

### 4.3 Node 和头文件传播

- `include/engine/inference_definition.h`：488 行
- `include/nodes/node_support.h`：294 行
- `include/core/session_context.h`：281 行
- `include/core/common_contracts.h`：279 行

其中 `node_support.h` 为了访问已校验端口计划而包含完整 `pipeline_validator.h`。建议将 `ValidatedNodePlan` 和端口绑定视图移至轻量契约头，降低所有 Node 翻译单元的传递依赖。

### 4.4 测试代码

`tests/unit/nodes/test_common_nodes.cpp` 为 758 行，同时项目已经存在逐节点测试文件。应保留真正的跨节点契约测试，将重复单节点行为迁移或删除，以减少编译和维护成本。

## 5. 构建与交付问题

### 5.1 SDK target 的公共接口不完整

`nlohmann_json` 被 `alg_sdk` 以 `PUBLIC` 传播：[`CMakeLists.txt`](../CMakeLists.txt#L82)，但 SDK target 没有完整的公共 include/install 接口，也没有发现 `install()`、CMake export 或 package config。

如果对外交付仅承诺 C ABI，应将 JSON 和内部 C++ 依赖保持为 PRIVATE。如果还需要正式 C++ authoring API，应建立独立 target 和明确的安装头集合。

### 5.2 第三方依赖构建会污染全局状态和源码树

[`cmake/ThirdPartyEngines.cmake`](../cmake/ThirdPartyEngines.cmake#L160) 使用 `FORCE` 修改 `BUILD_SHARED_LIBS`、PIC 和多个 ggml/llama 全局 cache 变量。下载或编译完成后，还会把依赖复制回源码树的 `3rdparty/`。

建议：

- 将依赖缓存放到可配置的外部 cache 或 binary directory。
- 使用依赖子构建，避免修改主工程全局变量。
- 用 CMake Presets 明确 lightweight、full、sanitizer 和 release 配置。

### 5.3 增量构建残留旧版本共享库

当前 `build/` 同时残留：

- `libcompany_alg_sdk.so.4.3.0`
- `libcompany_alg_sdk.so.8.0.0`
- `libcompany_alg_sdk.so.10.0.0`

这些文件不参与当前链接，但打包流程如果使用 glob，可能误收旧版本。应保证 release/CI 使用干净构建目录，并让打包清单来自目标属性而不是目录匹配。

## 6. 符合架构设计的部分

- 六个 C ABI 函数都保留了 `noexcept`、`catch (const std::exception&)` 和 `catch (...)` 屏障：[`src/adapter/company_c_adapter.cpp`](../src/adapter/company_c_adapter.cpp#L21)。
- 业务转换集中在 Adapter 和 Operator bridge，没有发现中央业务分派 switch。
- Pipeline 在物化运行时对象前完成校验，并消费 `ValidatedPipelinePlan`。
- 生产 Node 均通过 Definition 注册机制注册。
- Model 和 Backend 语义总体分离，vendor 头基本局限在具体 Backend。
- BGE embedding、BGE reranker 和 Qwen 固定批路径均使用 `FixedBatchExecutor`。
- AlgContext 实现了线程安全的不可变快照和写一次发布。
- 同一 handle 的 Process/Control 串行化、节点所有权和固定批 provenance 已有专项测试。

这些基础说明项目不需要推倒重来，重点是把已有设计意图变成编译期和门禁可验证的约束。

## 7. 验证结果与局限

执行了项目规定的完整门禁：

```bash
./scripts/run_all_tests.sh
```

结果：

- 构建成功
- 85/85 tests passed
- 0 tests failed
- 总耗时 121 秒
- LayerGuard、C11 ABI、Catalog、Pipeline、Node、Adapter、CLI 和文档门禁均通过

现有门禁通过说明当前正向路径和既有负例稳定，但不能否定本报告的问题：版本漂移、ABI 符号暴露和业务入口覆盖均不在当前门禁覆盖范围内。

本次未进行真实生产模型、目标硬件性能、长期压力、TSAN/ASAN 或故障注入验证，因此报告中的性能影响属于结构性判断，而不是基准测试结论。

## 8. 建议整改顺序

### 第一阶段：发布阻断项

1. 修复 Validator 对业务入口键覆盖的漏检，并增加负例。
2. 建立产品版本单一事实源和一致性测试。
3. 收紧共享库可见性，建立 ABI 导出 allowlist。

### 第二阶段：强化架构边界

1. 提取轻量 contracts/SPI。
2. 明确独立 Composition Root。
3. 建立可表达依赖方向的 CMake target graph。
4. 拆分业务 Blackboard keys 与中立 capability types。
5. 引入类型安全的 SessionResourceKey。

### 第三阶段：降低维护和构建成本

1. 冻结 Catalog 或改为不可变快照。
2. 拆分 Core 内部规则实现，但保留单一 Validator 门面。
3. 拆分 Operator 值类型注册中心。
4. 精简重复 Node 测试和非正式公共模板。
5. 建立 install/export/package、干净发布构建和第三方缓存策略。

## 9. 最终判断

当前架构的职责分区值得保留，主要问题不是抽象方向完全错误，而是设计约束尚未转化为编译边界、ABI 边界和自动化门禁。项目尚未投产，正是处理这些问题成本最低、收益最高的阶段。
