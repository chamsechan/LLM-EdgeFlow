# LLM-EdgeFlow 框架简化与跨请求状态实施计划

> 基线：`main@5140195`（2026-09-29）。本文件是工作计划，不是现行规则文档。
> 按 `CONTRIBUTING.md` §3，不把它作为提案归档提交到 `doc/`：每个阶段的设计写进对应 PR 描述，
> 落地后的规则写进各自的现行指南。

## 1. 目标与约束

**目标**

- 做成通用框架：业务开发者只需要补业务逻辑（Node 函数、转换器的 Decode/Encode 函数），再写 Pipeline JSON 编排。
- 节点可以跨业务复用，多个节点组合成一个业务。
- 降低上手门槛，去掉框架中不必要的复杂度。

**硬约束**

| 约束 | 来源 |
| --- | --- |
| 四层架构及依赖方向不变 | 用户确认 |
| 平台契约不变：Operator API、宿主结构体、槽位名、输出池语义、Demo 与 SDK 的交互、`.conf` 格式 | 用户确认 |
| 节点写法保留"函数 + Spec"，不引入派生基类 | 讨论结论：两者心智基本一致，再加一层只会多一套写法 |
| 沿用现有概念和命名（InputConverter、OutputConverter、IoBinding、`biz_name`、`io_binding` ID），不新造同义概念 | 用户要求 |
| 每项都要有明确收益，不做高投入低收益的事 | 用户要求 |

**已确认的决策**

| 问题 | 决定 |
| --- | --- |
| `biz_name`、`io_binding` ID 是否属于平台契约 | 不属于；保留现有命名，便于理解 |
| Exposure 白名单是否必须由代码控制 | 不需要，删除 |
| `.conf` | 平台定义，不动 |
| 跨请求状态的范围 | 每个 Operator 句柄一份 |
| 同一批次内的请求能否看到彼此的写入 | 不能，批次内各请求是平行的 |
| 进程重启后是否保留 | 不保留 |
| 状态接口的形式 | 通用接口：既支持累积（如 embedding 历史），也支持切换（如某个模式一直保持到下一个特殊请求） |
| BizDefinition 是否改为由 Binding 和转换器派生 | 不改，保留独立声明（原阶段 3b 取消，理由见第 9 节） |
| 阶段 3 实施前要补齐什么 | 扩展接口迁移清单、删除 Exposure 后的检查范围（见 7.2、7.3） |
| 阶段 4 的推进方式 | 先补齐设计和验收，评审确认后再实施 |
| 状态的提交边界 | 状态候选和输出发布对象全部准备成功后才最终提交；多个状态一起生效，或一起保持旧值 |
| 状态的数据所有权 | 从产生起就是共享不可变对象；提交只转移或共享所有权；已有读取指针始终有效；所有读取路径对初始空状态的处理一致 |
| 状态与请求数据如何配合 | 规定哪些端口可以直接读状态、哪些必须先广播到每条请求；新状态由写入节点生成，批内冲突指令由业务逻辑明确裁决 |
| 状态容量与成本 | 每个状态必须声明容量上限，历史类状态有明确的淘汰规则；内存峰值、每批耗时、"读取不复制"和"更新成本"都进入验收 |
| 真实业务闭环 | 先做"模式保持"，再做"向量历史" |

## 2. 现状核对

| 发现 | 证据 | 阶段 |
| --- | --- | --- |
| 新增 Node、转换器、Binding、Demo 文件都要手工登记 CMake。漏登记时文件不编译、注册静默缺失，到校验阶段才报 `Unknown node_type` | `src/custom_nodes/`、`src/common_nodes/`、`src/adapter/`、`demo/` 的 CMakeLists；脚手架为此专门提供 `--add-to-cmake`；测试目录已用 `file(GLOB ... CONFIGURE_DEPENDS)`（`tests/RuntimeTests.cmake:41`） | 1 |
| Spec 只有 Map 函数有签名检查；Batch 的 `Run` 和 LLM 钩子签名写错时，报错出现在模板深处 | `include/nodes/function_node.h:96` 有检查；`InvokeBatch`（`:120`）、LLM 钩子（`:1603`、`:1641`）没有 | 2 |
| 同一个批次上限 64 写在 4 处：输入转换器、输出转换器、Exposure、Binding，运行时取最小值 | 15 个转换器 + 8 个 biz 文件；`src/adapter/deployment_preparation.cpp:140-149` | 3 |
| Exposure 只是在 Binding 之外把业务名和批次上限再声明一次 | `include/adapter/io_binding.h:30-33`；唯一消费点是 `deployment_preparation.cpp:145-149` 和 `src/adapter/io_binding_registry.cpp:414-427` | 3 |
| 端口在转换器 `logical_ports`、BizDefinition ingress/egress、Binding 端口表中写 3 遍。8 个 Binding 里只有 1 处不是同名映射 | `src/adapter/biz/compliance_audit_bindings.cpp:51`；审计代码 `io_binding_registry.cpp:244-377` 专门比对这三份声明 | 3 |
| 转换器注册时拒绝 `max_batch_size == 0`；测试夹具普遍依赖"Binding 为 0 时继承转换器上限" | `src/adapter/io_converter_registry.cpp:78`、`:165`；`tests/unit/adapter/test_io_binding_registry.cpp:130-139` 与 `EffectiveBatchLimitIncludesBindingBound` 测试 | 3 |
| 没有跨请求状态。会话缓存 `GetOrCreateResult` 只能创建一次，没有更新和淘汰；端口 `lifetime=session` 只是元数据 | `include/nodes/session_resources.h`；`pipeline_validator.cpp:68-84` | 4 |
| 同一句柄上的 `Process` 已经被句柄互斥锁串行化 | `src/adapter/operator/operator_adapter.cpp:321` | 4 |
| `AlgContext` 按值存储（`std::any`），直接发布历史状态会每个批次复制一次 | `include/core/alg_context.h:43-47` | 4 |
| 读取入口不止一个：`BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require` 都直接调用 `AlgContext::Read` | `include/nodes/node_base.h:65`、`:73`、`:301` | 4 |
| `TextTemplateNode`、`PromptGuidedLlmNode` 的上下文输入按请求聚合；共享条目（`req_id=0`）直接接进去只会挂到批内第一条请求上 | `src/common_nodes/text_template_node.cpp:525-534`；`src/custom_nodes/prompt_guided_llm_node.cpp:198` | 4 |
| 现有的共享数据约定只有 `req_id=0`，由消费方用配置开启（`VectorTopKNode` 的 `candidate_scope=shared`）；端口定义里没有"共享输入"的声明 | `src/common_nodes/vector_top_k_node.cpp:77-83`、`:164-173`；来源规则的允许值见 `src/core/node_definition_validation.cpp:17-21` | 4 |
| 输出发布在一个函数里边构造共享指针边写入输出，没有"准备"与"提交"之分。内存池会拒绝重复归还，所以中途失败不会损坏账本 | `src/adapter/operator/operator_process_binding.cpp:232-252`；`src/adapter/operator/operator_output_pool.cpp:134-151` | 4 |
| 测试夹具里有维度可配置的测试向量模型；Demo 在同一个句柄上按 `batch_size` 分批处理数据集 | `dev_support/inference/test_biz_models.cpp:83-92`；`demo/common/operator_runner.h:215-271` | 4 |
| Studio 会就地修改 Pipeline 对象，但按数据名自动推导连线 | `tools/pipeline_studio/web/workbench.js:65-120` | 4 |

## 3. 总览与顺序

| 阶段 | 内容 | 新概念 | 规模 | 设计审查 | 建议分支 |
| --- | --- | --- | --- | --- | --- |
| 0 | 记录基线 | 无 | 极小 | 不需要 | 不建分支 |
| 1 | 业务源码目录自动收录 | 无 | 小 | 不需要 | `chore/auto-collect-sources` |
| 2 | Spec 签名编译期提示 | 无 | 小 | 不需要 | `refactor/spec-signature-diagnostics` |
| 3 | Adapter 重复声明收敛：删除 Exposure、批次上限只写一处、同名端口免写 | 无（净减少 Exposure） | 中 | 需要（扩展接口变更，见 7.2） | `refactor/adapter-binding-dedup` |
| 4 | 跨请求状态 | `state`；端口来源规则新增 `shared` 取值 | 中偏大 | 需要（Pipeline 结构、所有权、提交边界），先评审第 8 节 | `feat/pipeline-state` |
| 5（已完成） | 平台与调度参数归位：业务作者不再手填平台限制和调度参数（分 5.1–5.5 五步） | 无 | 小（5.1–5.3）＋中（5.4、5.5） | 5.4、5.5 需要 | `feat/auto-serialize-parallel-layers` |

**顺序理由**

- 阶段 1、2 相互独立、风险低，先做可以立刻降低日常摩擦；阶段 3、4 新增的源文件和测试也就不用再登记。
- 阶段 3 放在 4 之前，是因为阶段 4 的集成测试要通过 Operator 运行，先让接入层稳定下来。
- 阶段 3、4 之间没有代码依赖。如果需要先交付新能力，可以对调。
- 阶段 5 与阶段 4 没有代码依赖；5.4 与阶段 4 都会改接入层的解码、编码和输出发布路径，同时进行时先合并其中一个。

**每个阶段的通用流程**

1. 从最新 `main` 建分支。
2. 需要设计审查的阶段，先在 PR 描述中写清：问题、方案、受影响的契约、取舍、验收标准、回退方式（`CONTRIBUTING.md` §3）。
3. 实现代码，并补充聚焦测试。
4. 更新受影响的现行文档；用户能感知到的变化写进 `doc/CHANGELOG.md`。
5. 运行 `./scripts/run_all_tests.sh`，必须通过。
6. 按本阶段的"验收"清单逐项检查，把结果记录在 PR 中。验收通过后，再进入下一阶段。

## 4. 阶段 0：记录基线

在 `main` 上构建完成后，保存后续对比要用的数据：

```bash
./build/alg_pipeline_tool catalog > build/catalog-before.json
./build/alg_demo --suite smoke --output-dir build/smoke-before
```

阶段 1、3 的验收都会与这两份结果做对比。

## 5. 阶段 1：业务源码目录自动收录

**收益**

- 新增 Node、转换器、Binding、Demo 文件后，直接构建即可进入 Catalog，不用再改 CMake。
- 消除"文件没编译 → 注册缺失 → 到校验时才报错"这类难排查的问题。
- 脚手架去掉 `--add-to-cmake`，入门教程少一步。

**改动**

1. `src/custom_nodes/CMakeLists.txt`、`src/common_nodes/CMakeLists.txt`（含 `support/`）：改用 `file(GLOB ... CONFIGURE_DEPENDS)` 收集 `*.cpp`。仍然写成 `target_sources(edgeflow_capability_nodes_objects PRIVATE ${...})`，这样 `scripts/check_layer_isolation.sh` 的归属检查继续成立。
2. `src/adapter/CMakeLists.txt`：只把 `input/`、`output/`、`biz/` 这三个业务目录改为自动收录。框架机制文件（`deployment_*`、`io_*`、`operator/`）保持显式列表。
3. `demo/CMakeLists.txt`：`biz/` 目录改为自动收录。
4. `src/core`、`src/engine` 不改。Backend 按构建选项条件编译，需要保持显式列表。
5. `scripts/scaffold_custom_node.py`：删除 `--add-to-cmake` 参数，以及对应测试（`tests/tooling/test_scaffold_custom_node.py:65`、`:81`）。项目不保留旧参数别名。
6. 文档中删除 `--add-to-cmake` 和"登记 CMake"步骤，涉及：
   - `doc/dev_guide/first_custom_node.md`（`:50`、`:108`、`:175`）
   - `doc/dev_guide/first_control.md`（`:28`、`:40`）
   - `src/custom_nodes/README.md`（`:21-24`、`:30`）
   - `tests/README.md:59`
   - 三个 Node skill：`edgeflow-node-map-developer`、`edgeflow-node-llm-developer`、`edgeflow-node-batch-developer`

**验收**

- [ ] 改动后导出 `build/catalog-after.json`，其中节点类型、输入/输出转换器 ID、Binding ID 的集合与基线完全一致。
- [ ] 用脚手架在 `src/custom_nodes/` 生成一个临时节点，不改 CMake，重新构建后 `describe-node` 能查到它；删除文件并重新构建后查不到。过程记录在 PR 中，临时文件不提交。
- [ ] 在仓库中 `grep -rn "add-to-cmake"` 没有任何结果。
- [ ] 门禁通过（门禁包含 `check_layer_isolation.sh`）。

**回退**：还原 CMakeLists 和脚手架相关的提交。

## 6. 阶段 2：Spec 签名编译期提示

**收益**：节点作者最常写的是 Batch 的 `Run` 函数和 LLM 钩子。签名写错时，报错从一大段模板实例化信息，变成一句"期望的签名是什么"。

**改动**（`include/nodes/function_node.h`）

1. `InvokeBatch`（`:120`）：用 `static_assert` 列出可接受的签名，并要求返回类型是 `NodeResult<输出类型>`。可接受的签名为：
   - `Run(const Inputs&, const Params&)`
   - 在此基础上加 `const Models&`
   - 再加 `const SessionResources&`
   - 以及对应的成员函数形式
2. `MakeLlmTextSpec` 的 `BuildPrompt`、`FormatAnswer`（`:1603`、`:1641`）：参数只能是 `(const std::string&)` 或 `(const std::string&, const Params&)`，返回类型只能是 `std::string` 或 `NodeResult<std::string>`。
3. 断言放在 Spec 工厂函数里，让报错定位在作者自己的调用处。
4. 新增编译失败测试：准备若干只包含错误签名的小源文件，由 CTest 调用当前编译器（`-fsyntax-only`），断言编译失败，并且输出中包含对应的断言文本。测试标签要满足 `tests/contract/architecture/test_test_labels_contract.py`。

**验收**

- [ ] 以下四种错误各有一个编译失败用例，并且都通过：Batch 参数错、Batch 返回类型错、`BuildPrompt` 签名错、`FormatAnswer` 返回类型错。
- [ ] 现有全部节点和 `dev_support/node_authoring/` 下的 starter 编译不受影响。
- [ ] 门禁通过。

**不做**：不改 Spec 的写法本身，包括链式钩子、拆分头文件、`PortFlow` 改枚举。

## 7. 阶段 3：Adapter 重复声明收敛

### 7.1 收益与原则

| 同一个事实 | 现在 | 之后 |
| --- | --- | --- |
| 批次上限 | 4 处 | 生产代码只在 Binding 写 1 处；转换器只在确有自身限制时才声明 |
| 端口表 | 3 处 | 2 处（转换器 `logical_ports`、BizDefinition）；Binding 只写不同名的映射 |
| 业务名 | BizDefinition、Exposure、Binding 各一次 | BizDefinition、Binding 各一次 |
| 每个 biz 文件的内容 | 3 个结构 + 一个自执行注册 lambda | 2 个结构（BizDefinition、`IoBindingDefinition`）+ 原有注册样板 |

现有业务的节点、Pipeline 配置、`.conf`、Operator 接口和 Demo 都不变，只删重复的声明和参数。

**与现行规则的关系**：RFC 0066（提交 `9c64981`）有两条相关规定，本阶段都遵守，PR 设计说明中要写明：
- "不同来源的限制仍独立存在，运行时继续执行其交集"：输入转换器、输出转换器、Binding 三处来源都保留，都改为可选，按正值取交集，平台池深照旧参与。删掉的只是重复写的 64，以及已确认不需要的 Exposure。
- "BizDefinition 独立声明"：保持不变，不从转换器推导。

### 7.2 扩展接口迁移

**兼容策略**
- 这些接口属于源码扩展头。`doc/dev_guide/source_layout.md` 写明它们"需要随框架重新编译，不承诺内部 C++ 动态 ABI"，项目也不保留旧别名。因此直接修改，并同步迁移仓内全部调用方。
- 删除的接口会让调用方编译失败，由编译错误引导迁移；静默的语义变化写进 CHANGELOG 的迁移说明。
- SDK 公共头（`include/edgeflow/`）、Operator ABI（major 9）和产品版本号都不变。

| 扩展接口 | 变化 | 调用方如何发现 | 仓内迁移 |
| --- | --- | --- | --- |
| `BizExposureDefinition`、`REGISTER_BIZ_EXPOSURE`、`IoBindingRegistry::RegisterExposure/FindExposure/AllExposures` | 删除 | 编译错误 | 8 个 biz 文件删掉 Exposure；测试 `test_operator_safety.cpp:472-484`、`test_complex_converters.cpp:489`、`test_io_binding_registry.cpp`（保存与恢复 Exposure、`AuditRejectsMissingProductionExposure` 等用例） |
| `InputConverterDefinition::max_batch_size`、`OutputConverterDefinition::max_batch_size` | 默认值从 64 改为 0；0 从"注册失败"改为"转换器自身不设限"；非 0 仍参与取交集 | 语义变化，写进 CHANGELOG | 16 个转换器（7 个输入文件中的 8 个输入转换器，加 8 个输出转换器）删除 `= 64` 和 `kMaxBatchSize`；`io_converter_registry.cpp:78`、`:165` 不再拒绝 0；`test_io_converters.cpp:183-186` 由"拒绝 0"改为"接受 0" |
| `IoBindingDefinition::max_batch_size` | 0 的含义从"继承转换器和 Exposure"改为"Binding 不设限"；Binding、输入转换器、输出转换器三处全为 0 时，审计和部署准备都报错 | 语义变化，写进 CHANGELOG；全为 0 时有明确报错，且部署准备的报错先于 Validator 诊断 | 8 个生产 Binding 保留 64；自带非 0 上限的测试转换器不用改；复用生产转换器又没写上限的临时 Binding 要补 `max_batch_size`（仓内一处：`test_pipeline_catalog_validator.cpp` 的合成 Binding，由门禁发现） |
| `IoBindingDefinition::input_ports/output_ports` | 可以省略同名项，按转换器 `logical_ports` 补全；显式写出的项仍必须是转换器声明过的端口 | 向后兼容，写全仍然有效 | 8 个生产 Binding 删除同名项，只有 `compliance_audit` 保留 `matched_policies → matched_policy` |
| 直接读取 `input_ports/output_ports` 当作完整映射的代码 | 同名项不再写出，完整映射要用新增的 `EffectivePortMapping(declared, logical_ports)` 计算 | 语义变化：旧代码会少映射同名端口，解码时找不到 key | 部署准备、审计、Catalog 输出和 `alg_pipeline_tool validate-io` 改用有效映射；测试 `test_adapter_purity.cpp:1032`、`:1047` 同步修改 |
| `InputDecodeOptions` | 新增 `max_batch_size`，由 Operator 填入本句柄的有效上限；0 表示不检查上限 | 向后兼容 | `operator_adapter.cpp:353-355` 填值 |
| `ValidateDecodeRequest`、`DecodeRequestRows` | 删除批次上限参数，改为读取 `options.max_batch_size` | 编译错误 | 4 个多槽转换器（`doc_query`、`audit`、`rerank`、`image_query`）、3 个单行转换器文件（`text_input` 两处、`audio`、`translate_json`）、`test_io_converters.cpp:583`、`:619` |
| Catalog JSON 中转换器的 `max_batch_size` | 生产转换器由 64 变为 0 | 输出值变化，写进 CHANGELOG | 已核对：Studio 与工具测试只读取 `backend_config` 里的同名字段，不读这个值 |

### 7.3 删除 Exposure 后的检查范围

| 检查 | 现在 | 之后 | 位置 |
| --- | --- | --- | --- |
| 注册冲突（重复 ID、必填字段为空） | 有 | 不变 | 各注册表 |
| 转换器注册时拒绝 `max_batch_size == 0` | 有 | 删除（0 表示转换器自身不设限） | `io_converter_registry.cpp:78`、`:165` |
| 审计：Binding 引用的转换器已注册 | 有 | 不变 | `IoBindingRegistry::Audit` |
| 审计：转换器 `max_batch_size == 0` 报错 | 有 | 删除 | `io_binding_registry.cpp:238-243`、`:305-310` |
| 审计：Binding、输入转换器、输出转换器中至少有一处声明了正的批次上限 | 无 | **新增** | 审计与部署准备 |
| 审计：显式映射项必须是转换器声明过的端口 | 有 | 不变 | 审计 |
| 审计：转换器的必需端口都有映射 | 有 | 由同名补全自动满足 | 审计 |
| 审计：映射后的 key 必须存在于 BizDefinition 且类型一致，BizDefinition 的必需端口都被覆盖 | 有 | 不变，改为在补全后的有效映射上执行 | 审计 |
| 审计：同一业务的多个 Binding 外部契约一致 | 有 | 不变 | `CheckBizContract` |
| 审计：槽位的 ValueType 已注册 | 有 | 不变 | 审计 |
| 审计：每个 Exposure 至少有一个 Binding | 有 | **删除，不替代** | `io_binding_registry.cpp:414-427` |
| 部署准备：有效批次上限 | 输入、输出转换器，Binding（非 0 时）、Exposure（存在时）取最小值 | 三处中的正值取最小值；全为 0 时报 `DEPLOYMENT_ERROR`，路径 `/deployment/io/io_binding` | `deployment_preparation.cpp:140-149` |
| Operator：超过有效上限（与池深取小）的批次在解码前拒绝 | 有 | 不变 | `operator_adapter.cpp:314` |
| 解码辅助函数的防御性上限检查 | 使用转换器常量 | 使用 Operator 填入的有效上限 | `converter_authoring.h` |

**为什么删掉的检查不需要替代**：
- 那条检查的前提是存在一份"必须上线的业务"白名单，而白名单已经确认不需要。
- 现在可部署的业务就是已注册的 Binding，每个 Binding 在 SDK 初始化时都会完整审计。
- 配置引用了不存在的 Binding 时，部署准备会在处理任何请求之前报 `UNKNOWN_IO_BINDING`（`deployment_preparation.cpp:76-85`）。
- Exposure 原本参与的批次限制，由 Binding 的上限承担。当前两者都是 64，所以有效上限不变。

### 7.4 改动

1. 删除 Exposure 相关代码：
   - `BizExposureDefinition`、`REGISTER_BIZ_EXPOSURE`
   - `RegisterExposure`、`FindExposure`、`AllExposures` 及成员 `exposures_`
   - 审计中的 Exposure 一段
   - `deployment_preparation.cpp` 里与 Exposure 取最小值的代码
2. 批次上限按 7.2、7.3 调整：
   - 三处来源都改为可选，按正值取交集，全为 0 时报错；
   - 转换器注册接受 0；
   - 删除 16 个转换器里重复的 64；
   - 更新 `io_binding.h:23` 的注释。
3. `InputDecodeOptions` 增加 `max_batch_size`。`ValidateDecodeRequest`、`DecodeRequestRows` 删除上限参数，改读选项，由 Operator 在解码前填入。
4. Binding 的同名端口可以省略：
   - 补全后的有效映射由一个纯函数在使用时计算，部署准备、审计和 Catalog 输出都调用它；
   - 不在注册时改写 Binding，避免依赖跨文件的静态注册顺序；
   - Catalog 输出补全后的映射，因此内容与基线一致。
5. 8 个 biz 文件删除 Exposure 和同名端口表；Binding 保留 `max_batch_size = 64`。
6. 更新文档：
   - `doc/dev_guide/business_onboarding.md`（`:47`、`:58`、`:88`、`:105-108`、`:122-130`）
   - `doc/developer_guide.md:84`
   - `src/adapter/biz/README.md`
   - `src/adapter/input/README.md:9`
   - `edgeflow-adapter-developer` skill（`:26`）
   - `llm-edgeflow-developer-guide/references/integration.md:28-32`
   - `doc/architecture.puml:55`，并重新生成架构图
   - `doc/CHANGELOG.md`

### 7.5 改动后新业务的写法（以 keyword_match 为例）

```cpp
BizDefinition MakeKeywordMatchBizDefinition() {
  BizDefinition def;
  def.biz_name = kBizName;
  def.display_name = "关注词匹配";
  def.ingress = {RequiredBizInput(kRawRequestIds),
                 RequiredBizInput(kInputSentences)};
  def.egress = {BizOutput(kRuleMatches)};
  return def;
}
// BizDefinition 的注册样板保持现状

IoBindingDefinition MakeKeywordMatchOperatorBinding() {
  IoBindingDefinition def;
  def.binding_id = "keyword_match.operator.v1";
  def.biz_name = kBizName;
  def.input_converter_id = "keyword.plain.operator.v1";
  def.output_converter_id = "keyword.result.operator.v1";
  def.max_batch_size = 64;  // 生产代码中唯一的批次上限声明
  return def;
}
REGISTER_IO_BINDING(MakeKeywordMatchOperatorBinding());
```

与现在相比：没有 Exposure；没有 `kMaxBatchSize` 常量；Binding 不再列出同名端口；转换器不再声明批次上限。

### 7.6 验收

- [x] 与 `build/catalog-before.json` 对比：
  - `bizs` 完全一致；
  - `io_bindings`（包括补全后的端口映射）完全一致；
  - 唯一允许的差异是生产转换器的 `max_batch_size` 从 64 变为 0。
- [x] 8 个业务的有效批次上限仍是 64：用 `alg_pipeline_tool validate-io` 的 `effective_max_batch_size` 核对，并加单元测试逐一断言。
- [x] 在 `src`、`include`、`doc`、`.agents`、`tests` 中执行 `grep -rn "REGISTER_BIZ_EXPOSURE\|BizExposure\|kMaxBatchSize\|RegisterExposure"`，除 CHANGELOG 中的删除说明外没有任何结果。
- [x] 新增或更新以下单元测试：
  - 省略端口表时按同名映射；`compliance_audit` 的非同名映射；
  - 显式映射到转换器没有声明的端口时，审计报错；
  - Binding 与两个转换器的上限全为 0 时，审计和部署准备都报错；
  - 转换器声明了非 0 上限时，仍与 Binding 的上限取交集（对应 RFC 0066 的交集规则）；
  - 转换器 `max_batch_size = 0` 可以注册；
  - Binding 的端口与 BizDefinition 不一致时，审计仍然报错；
  - `InputDecodeOptions.max_batch_size` 生效：超出时解码辅助函数拒绝，为 0 时不检查上限。
- [x] `./build/alg_demo --suite smoke` 通过，逐条结果与 `build/smoke-before` 一致。
- [x] 文档按 7.4 第 6 项更新；CHANGELOG 写明 7.2 中所有接口变化和迁移方法。
- [x] 门禁通过。

**回退**：还原本阶段的提交即可。Pipeline JSON 和平台契约都没有变化。

### 7.7 实施记录（2026-09-30）

在分支 `refactor/adapter-binding-dedup`（基于 `main@5140195`）上实施，尚未提交。

| 验收项 | 结果 | 证据 |
| --- | --- | --- |
| Catalog 对比 | `backends`、`bizs`、`io_bindings`（含补全后的映射）、`models`、`nodes`、`profiles` 与基线一致；唯一差异是 16 个生产转换器的 `max_batch_size` 从 64 变为 0 | `build/catalog-before.json`、`build/catalog-after.json` |
| 有效批次上限 | 17 份配置的 `validate-io`、`resolve-conf` 输出与基线逐字节一致：可部署的配置都是 64；依赖 kite、whisper 的配置在改动前后以相同的错误失败 | `build/stage3-baseline/`、`build/stage3-after/`；`ComplexConvertersTest.AllEightBusinessesRegistered` |
| 残留名称 | 只剩 CHANGELOG 中的删除说明 | 7.6 的 grep |
| 单元测试 | 同名补全 `OmittedPortMappingsUseConverterPortNames`；非同名映射 `ComplianceBindingDeclaresOnlyRenamedPort`；未声明端口 `AuditRejectsMappingOfUnadvertisedPort`；全 0 上限 `AuditAndPreparationRejectBindingWithoutBatchLimit`；取交集 `EffectiveBatchLimitIncludesBindingBound`；转换器 0 上限 `IoConverterTest.RejectsInvalidDefinitions`；与 BizDefinition 不一致 `UnselectedIllegalBindingFailsAudit`；解码上限 `DecodeRowsUsesEffectiveBatchLimitFromOptions` | `tests/unit/adapter/` |
| Smoke | 9 个 Profile 的逐条结果与基线一致 | `build/smoke-before`、`build/smoke-after` |
| 文档 | 按 7.4 第 6 项更新；架构图重新生成，检查通过 | — |
| 门禁 | `./scripts/run_all_tests.sh` 通过：静态检查、构建、Tier 1–4 共 100 项 | `build/gate-stage3-final.log` |

**实施中发现的问题**
1. `AdapterPurityTest.ReuseProof_1` 把 `binding->input_ports` 当作完整映射使用，省略同名项后解码失败。已改用 `EffectivePortMapping`，并在 7.2 增加"静默语义变化"一行。
2. 门禁发现 `test_pipeline_catalog_validator.cpp` 的合成 Binding 复用了生产转换器，却没写上限，原先靠转换器的 64。改动后部署准备先报 "declares no batch limit"，盖住了 Validator 的 `UNKNOWN_BIZ`。夹具已补 `max_batch_size = 64`，7.2 和 CHANGELOG 已补迁移说明。
3. 批次上限规则由最初设想的"Binding 必须大于 0"改为"三处都可选、正值取最小、全为 0 报错"：转换器注册原先拒绝 0，部分测试夹具依赖从转换器继承上限。现规则改动面最小，也符合 RFC 0066 的交集规则。

## 8. 阶段 4：跨请求状态（设计与验收，评审确认后实施）

### 8.1 范围

- 用同一套机制支持两类场景：
  - 切换类：某个模式一直保持到下一个特殊请求；
  - 累积类：向量历史。
- 状态的作用范围是每个 Operator 句柄一份；同一批次内的请求读到同一份状态，互不可见对方的写入；进程重启后不保留。
- 不使用状态的 Pipeline 完全不受影响。
- 不做：按用户或会话 key 分区、持久化、通过 Control 清空状态、跨句柄共享（见 8.12）。

### 8.2 接口（Pipeline JSON）

```json
"state": {
  "mode":        { "type": "TextBatch",      "max_items": 1 },
  "vec_history": { "type": "EmbeddingBatch", "max_items": 10000 }
}
```

**声明**
- 名字：`[a-z][a-z0-9_]*`，最多 64 个状态。
- `type`：Catalog 中端口使用的 `type_id`，必须是可追溯批次类型（元素为 `TraceableItem`）。
- `max_items`：必填，范围 1–1,000,000，是框架强制的容量上限。

**读写**
- 读：节点**输入**接 `state.<名字>`，读到本批次开始时的版本。
- 写：节点**输出**接 `state.<名字>`，写入的是新状态的候选，按 8.5 的规则提交，下一个批次才能读到。
- `state.` 是保留前缀，不能用作普通数据名。

### 8.3 数据模型与所有权

**状态版本**
- 所有状态组成一个不可变的"状态集版本"：名字 → `std::shared_ptr<const std::any>`。
- 句柄的 `SessionContext` 持有当前版本（`std::shared_ptr<const StateSet>`）。

**初始值**
- 在 Pipeline 构建（节点初始化）时创建。
- 读或写某个状态的端口都知道它的 C++ 类型；第一个绑定该状态的端口创建该类型的空值，作为共享不可变对象放进初始版本。Validator 保证同一个状态只有一种类型。
- 因此运行时不存在"状态缺失"：`BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require`、`AlgContext::Read/Has` 看到的都是真实的值，不需要各自特殊处理初始空状态。

**批次快照**
- 批次开始时，本批上下文持有当前版本的一个引用，并把每个状态作为只读的共享条目挂进上下文。
- 读取不复制；读到的指针在整个批次内有效，即使期间有新版本提交也不受影响。

**写入候选**
- 写入节点发布状态输出时，框架先按 8.4 校验，然后直接把值构造成共享不可变对象（`std::make_shared<const std::any>`），放在上下文里一个保留的暂存 key（`state.<名字>.next`）下。
- 此后这个对象不再被复制或移动。

**提交**
- 以当前版本为基础，只复制指针，构造出新版本，然后替换当前版本。
- 所有权在上下文和新版本之间共享；旧版本在最后一个持有者释放后回收。

**并发保护**
- 准备提交时，校验本批的基础版本仍是当前版本（乐观校验）；版本已变化则准备失败。
- 在 Operator 内同一句柄是串行的，这种情况不会发生；这条校验防止有人在 Operator 之外并发执行同一个 Pipeline 时丢失更新。

**需要的框架改动**
- `AlgContext` 增加只读共享条目。`Read`、`Has` 同时查找普通值和共享条目；共享条目不能被覆盖，也不能与普通值同名。

### 8.4 状态与请求数据的配合规则

1. **状态值是句柄级共享批次**：所有条目 `req_id = 0`，`sub_id` 从 0 连续编号，条目数不超过 `max_items`。写入时由框架校验，违反时写入节点失败，本批不提交。
2. **直接读取**：只有"共享输入"端口可以直接接 `state.*`。
   - 端口来源规则（provenance）新增取值 `shared`，由节点作者在 Spec 中声明，例如 `PortFlow{"N:1", "shared"}`。
   - 现有的 `VectorTopKNode` 的 `candidates`、`candidate_texts` 通过配置切换：端口新增 `provenance_config_field` 字段（与现有 `lifetime_config_field` 同一机制），`candidate_scope` 为 `shared` 时，该端口按共享输入处理。
   - 接到非共享端口时，Validator 报 `PORT_PROVENANCE_MISMATCH`，提示"该端口按请求处理数据，需要先把状态广播到每条请求"。
   - 共享输入端口仍然接受现有的普通数据（例如语料向量），现有配置不受影响。
3. **需要广播**：
   - 按条对齐的输入（Map、LLM 文本）和按请求聚合的输入（如 `TextTemplateNode`、`PromptGuidedLlmNode` 的 `context`）不能直接接状态，否则状态只会挂到批内第一条请求上。
   - 需要按请求使用状态时，由节点调用框架提供的 `BroadcastToRequests(shared, anchor)`（放在 `nodes/traceable_batch_operations.h`），把共享条目复制到 anchor 中的每个请求上。
   - 纯 JSON 编排用的通用广播节点暂缓，见 8.12。
4. **写入**：
   - 写入节点根据"当前状态 + 本批数据"产出**完整的新状态**，不是增量。
   - 写入端口声明来源规则 `shared`。
   - 结果只能依赖本批数据及其顺序（批内顺序就是平台输入顺序）。
   - 同一批出现相互冲突的指令时，由写入节点的业务规则明确裁决，并写进节点说明和测试。模式保持业务采用"批内最后一条指令生效"。
5. **给发出指令的请求回执**：写入节点另开一个普通的按请求输出端口，不从状态读取。

### 8.5 成功与提交边界

`Operator_Process` 的顺序：

| 步骤 | 内容 | 失败时 |
| --- | --- | --- |
| 1 | 输入校验、输出槽解析 | 返回错误；没有租约，状态不变 |
| 2 | 解码 | 同上 |
| 3 | 租用输出块，由租约守卫接管 | 守卫归还已租用的块；状态不变 |
| 4 | 执行 Pipeline：开始时绑定当前状态版本；写入节点产出已校验的候选 | 同上 |
| 5 | 输出编码，写入已租用的块 | 同上 |
| 6 | **准备状态提交**：获取句柄状态锁；校验基础版本仍是当前版本、每个声明的状态都有且只有一个候选；构造新版本（只复制指针） | 同上，并释放锁 |
| 7 | **准备输出发布**：为每个输出块建好发布用的共享指针，但不写入 `outputs` | 同上 |
| 8 | **最终提交**（不会失败）：替换状态版本；把准备好的指针写入 `outputs` 中已存在的 key；提交租约 | — |

**规则**
- 第 8 步只包含不会抛异常的操作：共享指针交换、对已存在 key 赋值、租约守卫提交。
- 多个状态在同一个新版本里一起替换，要么全部生效，要么全部保持旧值。
- 不存在"状态已提交但输出没发布"或反过来的中间状态。

**接口**
- `Pipeline::Execute(AlgContext*)`：执行，并绑定快照。
- `Pipeline::PrepareStateCommit(const AlgContext&, std::string* error)`：返回持有锁和新版本的对象；该对象的 `Commit()` 声明为 `noexcept`；销毁时如果还没提交，则放弃。
- 不声明状态的 Pipeline 返回一个空操作对象。
- Operator 之外直接运行 Pipeline 的调用方（测试、工具）不调用提交，状态就不会前进，这是安全的默认行为。

**输出发布拆分**
- 把 `PublishOperatorOutputs` 拆成"准备"和"提交"两步。准备阶段失败时，只由租约守卫统一归还内存块。

**故障注入**
- 在第 6、7 步各加一个测试专用钩子，沿用现有 `Pipeline` 的 `test_internal_hook_` 做法。

### 8.6 校验规则（沿用现有诊断码，不新增）

| 规则 | 诊断码 |
| --- | --- |
| `state` 声明缺字段、字段类型错误、超出范围、名字不合法、类型不是可追溯批次 | `MISSING_FIELD` / `FIELD_TYPE` / `FIELD_RANGE` |
| 读写未声明的状态名；`state.` 前缀用于其他用途；接入边界（Binding 映射）使用 `state.*` | `UNKNOWN_FIELD` |
| 同一个状态有多个写入节点 | `DUPLICATE_PORT_PRODUCER` |
| 声明的状态没有写入节点；读取端口的类型与声明不符 | `MISSING_INPUT_PRODUCER`（与普通端口的类型不符报法一致） |
| 写入端口的类型与声明不符 | `INVALID_COMBINATION` |
| 读取端口不是共享输入；写入端口没有声明 `shared` | `PORT_PROVENANCE_MISMATCH` |

读取状态不产生依赖边；写入节点可以读取自己写的状态，不会被判为成环。`alg_pipeline_tool plan` 的输出增加 `states` 列表（名字、类型、容量、写入节点、读取节点）。Studio 不画状态连线，这是查看状态读写关系的入口。

### 8.7 各层改动

| 层 | 改动 |
| --- | --- |
| 编排层 | 配置结构增加 `state` 并解析（外部文档结构自动继承）；Validator 按 8.6 校验；`ValidatedPipelinePlan` 增加 `states`；`ResolvedPortBinding` 增加状态名和容量；来源规则允许 `shared`，并支持 `provenance_config_field`；`AlgContext` 增加共享条目；`SessionContext` 保存状态集版本；`Pipeline` 绑定快照，提供准备与提交接口 |
| 能力节点层 | 绑定到状态的 `BoundInput`/`BoundOutput` 在初始化时创建类型化的空值；写入时按 8.4 校验并构造共享对象；`VectorTopKNode` 的两个候选端口声明 `provenance_config_field = "candidate_scope"`；新增 `BroadcastToRequests` 辅助函数 |
| 接入适配层 | `Operator_Process` 按 8.5 调整；拆分输出发布；增加测试钩子 |
| 工具 | Studio 推导连线时跳过 `state.*`，保存时保留 `state`；`plan` 输出 `states`；导出的 JSON Schema 自动包含 `state` |
| 文档 | `configs/README.md` 的状态规则；`doc/architecture.md` 的状态版本与提交边界；`doc/dev_guide/custom_node_concepts.md` 的共享输入、广播与写入规则；`doc/developer_guide.md`；Studio README；CHANGELOG |

### 8.8 闭环一：模式保持（真实业务）

**业务**：关键词匹配（`keyword_match_v1`）加上模式保持。复用现有的 Binding、转换器和 Demo，不新增业务契约。

**规则**
- `#严格模式` 切到 strict，`#常规模式` 切回 normal。
- 同一批出现多条指令时，批内最后一条生效。
- normal 模式用常规规则，strict 模式用更严格的规则。
- 两套规则都把指令本身识别为 `MODE_COMMAND`，便于回执。

**配置**（`configs/pipeline_keyword_match_mode.json` 及对应 `.conf`）：

```json
{
  "state": { "mode": { "type": "TextBatch", "max_items": 1 } },
  "pipeline": [
    { "id": "mode_switch", "node_type": "ModeSwitchNode",
      "config": { "commands": { "#严格模式": "strict", "#常规模式": "normal" },
                  "default_mode": "normal" },
      "inputs":  { "texts": "input_sentences", "current": "state.mode" },
      "outputs": { "next": "state.mode" } },
    { "id": "rules_normal", "node_type": "TextRuleMatchNode",
      "config": { "categories": { "URGENT": ["加急"],
                                  "MODE_COMMAND": ["#严格模式", "#常规模式"] } },
      "inputs": { "text": "input_sentences" }, "outputs": { "matches": "normal_matches" } },
    { "id": "rules_strict", "node_type": "TextRuleMatchNode",
      "config": { "categories": { "URGENT": ["加急", "尽快"], "COMPLAINT": ["投诉"],
                                  "MODE_COMMAND": ["#严格模式", "#常规模式"] } },
      "inputs": { "text": "input_sentences" }, "outputs": { "matches": "strict_matches" } },
    { "id": "select", "node_type": "ModeSelectNode",
      "inputs":  { "mode": "state.mode", "normal": "normal_matches", "strict": "strict_matches" },
      "outputs": { "matches": "rule_matches" } }
  ],
  "deployment": { "io": { "io_binding": "keyword_match.operator.v1" } }
}
```

**新增两个自定义节点**（`src/custom_nodes/`，属于常规自定义 Node）
- `ModeSwitchNode`（写入节点）：
  - 输入 `texts`，以及共享输入 `current`；
  - 输出共享的 `next`，只有 1 条；
  - 规则：从当前模式出发（为空时用 `default_mode`），按批内顺序应用指令，最后一条生效。
- `ModeSelectNode`（直接读取状态）：
  - 输入共享的 `mode`，以及按请求的 `normal`、`strict` 两路结果；
  - 按本批快照的模式选择其中一路，输出与 `normal` 保持同序、同来源。

**Demo 与数据**
- 新增 Profile `keyword_match_mode`（`batch_size: 2`）和数据集 `tests/fixtures/effects/keyword_mode_inputs.txt`，并加入 Smoke 套件。
- 数据集的 7 行与期望结果（每 2 行一个批次）：

| 行 | 输入 | 所在批次读到的模式 | 期望结果 |
| --- | --- | --- | --- |
| 1 | 请尽快回复 | normal | 未命中 |
| 2 | #严格模式 | normal | `MODE_COMMAND` |
| 3 | 请尽快回复 | strict | `URGENT` |
| 4 | 我要投诉 | strict | `COMPLAINT` |
| 5 | #常规模式 | strict | `MODE_COMMAND` |
| 6 | 我要投诉 | strict | `COMPLAINT`（与第 5 行同批，仍读到 strict） |
| 7 | 我要投诉 | normal | 未命中 |

**复用情况**：`TextRuleMatchNode` 用了两次，Binding、转换器、Demo 运行代码都不改。新增的两个节点是这个业务真正需要的逻辑，可以借此检验接口是否好用。评审节点在 4c 结束时进行（见 8.11）。

### 8.9 闭环二：向量历史（验证容量、共享条目和成本）

**测试用 Pipeline**
- 使用测试向量模型：`TextEmbeddingNode` → `VectorTopKNode`（`candidate_scope: shared`，候选接 `state.vec_history`）→ 命中判定 → `rule_matches`。
- 另有写入节点，输入为历史状态和本批向量，输出新的历史。
- 写入节点和命中判定节点放在 `tests/` 中，作为测试夹具。

**容量与淘汰**
- 状态声明 `max_items`，由框架强制。
- 写入节点按先进先出淘汰最旧的条目，并在输出前把条目数收敛到上限以内。
- 输出超过上限时写入失败，本批不提交。

**成本测量**
- 新增 `dev_support/benchmarks/state_history_bench.cpp`，不进门禁，结果记录在 PR 中。
- 测量组合：`max_items ∈ {1,000; 10,000; 100,000}` × 向量维度 `{384; 768}`。
- 分别记录：
  - 快照绑定耗时；
  - 写入节点更新耗时（不可变状态下，追加需要构造新历史，预期与条目数成线性关系）；
  - 提交耗时；
  - 每批总耗时（p50/p95）；
  - 进程内存峰值（`getrusage`）。
- 理论内存上界：同一时刻最多存在当前版本和候选两份历史，约为 `2 × max_items × 每条字节数`。

### 8.10 验收

**Core 单元测试**
- [ ] 8.6 中每条规则都有对应的报错用例。另外验证：读取不产生依赖边；写入节点读取自己的状态不成环；`plan` 输出 `states`。
- [ ] 初始空状态：通过 `BoundInput::Get`、`BoundInput::Require`、`NodeBase::Require`、`AlgContext::Has` 读取，结果一致，都是真实存在的空值。
- [ ] 所有权：
  - 批内读到的指针在后续提交之后仍然有效，内容不变；
  - 用一个会计数复制次数的测试载荷类型，验证读取和提交的复制次数为 0。
- [ ] 写入校验：`req_id ≠ 0`、`sub_id` 不连续、超过 `max_items` 时，写入节点失败，状态不变。
- [ ] 共享输入规则：
  - 状态接到按请求聚合的端口、按条对齐的端口，都报 `PORT_PROVENANCE_MISMATCH`；
  - 接 `VectorTopKNode` 时，`candidate_scope=shared` 可以通过，`request` 报错；
  - 现有全部配置仍然通过校验。
- [ ] `BroadcastToRequests`：共享条目被复制到每个请求，`sub_id` 保持原顺序。

**提交边界集成测试**（通过 Operator 运行）
- [ ] Pipeline 中某个节点失败时，状态不变。
- [ ] 输出编码失败时（用极小的 `out_mem` 容量触发），状态不变。
- [ ] **输出发布准备失败**时（第 7 步钩子）：没有任何输出被发布，所有租用的块都回到空闲状态（按内存池账本核对），状态不变。
- [ ] **多状态提交准备失败**时（第 6 步钩子，Pipeline 声明两个状态）：两个状态都保持旧值。
- [ ] 两个状态都成功写入时，下一批同时看到两个新值，且来自同一个版本。
- [ ] 句柄隔离：同一配置的两个句柄，状态互不影响。

**闭环一：模式保持**
- [ ] 8.8 表中 7 行的结果全部符合期望，通过 Operator 集成测试和 Demo Profile 各验证一次；Profile 已加入 Smoke 套件。
- [ ] 冲突裁决：同批出现 `["#严格模式", "#常规模式"]` 时下一批为 normal；顺序反过来时为 strict。
- [ ] 在失败回滚和句柄隔离场景下，也使用这个业务的配置再验证一次。

**闭环二：向量历史**
- [ ] 第一批读到空候选，不报错；批 1 的 `"A"` 未命中，批 2 的 `"A"` 命中，`"B"` 未命中。
- [ ] 历史中的条目都是 `req_id=0`，`sub_id` 连续；达到容量后淘汰最旧的条目，条目数始终不超过 `max_items`。
- [ ] 成本测量结果写进 PR，并满足：
  - 快照绑定和提交耗时与条目数无关（1,000 与 100,000 条之间相差不超过 2 倍）；
  - 更新耗时单独列出；
  - 内存峰值增量不超过理论上界的 1.25 倍。

**其他**
- [ ] Studio：含 `state` 的方案可以打开、保存、校验，保存后 `state` 字段仍在；图中不出现状态连线；导出的 JSON Schema 包含 `state`。
- [ ] 现有全部配置和 Smoke 套件的行为不变；门禁通过；8.7 列出的文档全部更新。

**回退**：`state` 是可选的新增字段；按 8.11 的子步骤分别提交，任一子步骤都可以单独回退。

### 8.11 实施顺序（评审确认后）

| 子步骤 | 内容 | 结束时的检查点 |
| --- | --- | --- |
| 4a | 编排层与节点层的运行时及校验（不含 Operator） | Core 单元测试全部通过 |
| 4b | Operator 提交边界与输出发布拆分 | 提交边界集成测试全部通过 |
| 4c | 闭环一：模式保持 | 业务测试与 Demo 通过。**在这里暂停评审接口是否好用**，必要时先调整接口，再进入 4d |
| 4d | 闭环二：向量历史与成本测量 | 容量与成本验收通过，门禁通过 |

### 8.12 本阶段不做

- **纯 JSON 编排用的通用广播节点、通用"追加并限制容量"节点**：两个闭环都不需要。等第一个真实业务需要时，再按新增通用 Node 的流程（设计审查）加入。
- 按用户或会话 key 分区、持久化、通过 Control 清空状态、跨句柄共享、在状态上配置 merge 或 evict 规则。

## 9. 不做的事项及理由

| 提议 | 结论 | 理由 |
| --- | --- | --- |
| BizDefinition 改为由 Binding 和转换器派生（原阶段 3b） | 不做 | 不补任何能力。做完阶段 3 后，剩下的重复只是端口清单列两次；两边引用同一批常量，漏改会被审计报出。派生必须等所有转换器注册完，但 `alg_pipeline_tool catalog`（`src/tools/alg_pipeline_tool.cpp:308`）和契约测试（`tests/contract/catalog/test_catalog_contract_ssot.cpp:78`）会在此之前读取业务契约。可靠的修法是给每个入口加"完成注册"调用，或在 Core 新增挂接机制，都会增加框架复杂度。此外还会失去"改转换器端口时审计报错"这道检查，并推翻 RFC 0066（`9c64981`）刚定下的规则。实测 8 个业务的派生结果与手写定义逐字段一致；将来如果接入量大到这处重复成为负担，再单独设计 |
| 用派生基类写节点 | 不做 | 与 Spec 的心智基本一致（`MakeLlmTextSpec` 本身就是"填钩子"），加一层只会多一套写法 |
| 具名链式钩子、公开通用节点的 Spec、拆分/过滤/排序预设工厂、节点分类树、`PortFlow` 改枚举 | 不做 | 都是新写法或新概念。报错可读性的问题由阶段 2 解决；`PortFlow` 的非法值在注册时已经会被校验 |
| 把接线从 C++ 挪到 JSON `deployment.io` | 不做 | 需要同时改 Validator、Catalog、Studio、SDK 初始化和全部配置。阶段 3 在 C++ 内合并已经拿到主要收益 |
| `REGISTER_DECODER/ENCODER`、`RowInputConverter` 等新名字 | 不做 | 与现有 InputConverter、OutputConverter 同义 |
| 由函数签名推导转换器的宿主类型和槽位 | 不做 | `ExternalInputSlot<T>`、`ExternalOutputSlot<T>` 已经由类型推出类型名；槽位名是平台契约字符串，只需写一行，收益小 |
| Demo 按载体重组 | 不做 | 载体是固定的，而且每个载体都已有 Demo；新业务复用现有运行函数只需一行 `REGISTER_DEMO_BIZ` |
| 模型配置拆分、节点预设、子图 | 不做 | 都是新的 JSON 概念，成本高 |
| 配置变体一致性测试 | 不做，交给方案负责人 | 现有差异里，按后端调整的生成参数和模板可能是有意的，规则内容的差异可能是遗漏，这需要业务判断，不是框架机制的问题。已发现的差异见附录 |
| 状态的 merge、evict 等配置项 | 不做 | 合并与淘汰规则由写入节点实现；框架只强制容量上限 `max_items` |
| 纯 JSON 用的通用广播节点、通用追加节点 | 暂缓 | 两个闭环都不需要，等第一个真实业务需要时再加（见 8.12） |
| 统一错误类型、大规模改写文档 | 不做 | 各阶段只更新受影响的文档 |
| 部署配置与业务编排分离（按硬件拆出 `models`、`backend_config`，业务 Pipeline 只留一份） | 不做 | 改变 Pipeline JSON 格式，属于外部契约；与"模型配置拆分"同理。5.1 和 5.3 的默认值清单已能去掉大部分重复 |
| 合并 `model_config.max_batch_size` 与 `backend_config.max_batch_size` | 不做 | 现有配置写了这两个字段，而未知字段会被拒绝；删掉任一字段都会让现有配置失效 |
| 新增 SDK 接口查询有效批次上限 | 不做 | 改变公共 Operator 接口；有效限制改由 `resolve-conf` 查询（5.3） |
| Demo 运行前读取内部有效限制并自动分批 | 不做 | Demo 应和真实宿主一样只通过 SDK 运行；超限时 Create/Process 已明确报错 |
| 批量删除配置中等于默认值的显式字段 | 不做，交给方案负责人 | 显式值可能是有意固定，现在无法区分（Studio 曾自动写入默认值，已由 5.1 修复，PR #150）。现有配置保持原样，清单待方案负责人确认（见第 12 节） |
| `validate` 对"显式值等于默认值"给出警告 | 不做 | 与"保留有意固定的显式值"矛盾；根源由 5.1 修复 |

## 10. 核对代码后对前几轮讨论的修正

| 之前的说法 | 修正 | 依据 |
| --- | --- | --- |
| 写状态的 Pipeline 需要在句柄上按批次串行 | 不需要新增机制 | `operator_adapter.cpp:321` 已按句柄加锁 |
| 从未写入的状态，在读取入口返回静态空值 | 改为在构建时创建该类型真实的空值，所有读取路径自然一致 | 读取入口不止 `BoundInput::Get`，还有两处 `Require` 和 `Has` |
| Pipeline 执行和输出编码都成功才提交 | 改为：状态候选和输出发布对象全部准备成功后，才一次性提交；多个状态一起生效或一起保持旧值 | 用户要求；原设计没有覆盖"输出发布准备失败"和"多状态"的情况 |
| 状态声明只写类型 | 改为必须声明 `max_items` | 历史类状态必须有明确的容量上限 |
| 任何输入端口都可以读状态 | 改为只有共享输入端口可以直接读，其他端口必须先广播 | 按请求聚合的端口会把状态挂到第一条请求上 |
| 测试写入节点全部放在 `tests/` | 改为模式保持做成真实业务，放在 `src/custom_nodes/`；向量历史的节点仍放在测试中 | 用户要求用真实业务闭环检验接口 |
| Binding 的批次上限改为必填 | 改为三处来源都可选，按正值取交集，全为 0 时报错；生产代码只在 Binding 中写 | 转换器注册拒绝 0，测试夹具依赖继承；强制必填会带来大量无关改动 |
| 转换器的宿主类型和槽位由签名推导 | 撤回 | 见第 9 节 |
| 配置变体一致性测试 | 撤回 | 见第 9 节 |
| BizDefinition 并入 Binding（原阶段 3b） | 撤回 | 见第 9 节。不必要，而且按原写法"首次读取时派生"，会让 `alg_pipeline_tool catalog` 输出空的 `bizs`，并让现有契约测试失败 |
| 批次上限 64 是引入转换器时（`7f8f0b1`）定下的 | 首次出现在 `43c777f` / `0a27334`（2026-08-19），随 fail-closed 批次契约引入，提交说明没有给出取值依据 | `git log -G "max_batch_size\s*=\s*64"` |
| 删掉配置中与默认值相同的字段即可 | 改为先修复 Studio（5.1），存量字段列清单交方案负责人（5.3） | 显式值可能是有意固定 |
| 并行时的拒绝等第一个并行 Pipeline 出现再改 | 阶段 5.5 已按约束自动分层，兼容放宽已确认 | 普通开发者不应管理并行调度；改动只在 Validator，执行器已支持单节点层顺序执行 |
| Node 编写一侧已没有平台参数 | 不完整：Studio 会把 Node 参数默认值写成显式值；Control 命令 ID 需要人工选号 | `tests/tooling/studio_config_roundtrip_test.mjs`（PR #150）；`doc/dev_guide/first_control.md:23` |
| 去掉默认值字段后 `plan` 输出一致，所以配置等价 | `plan` 只输出拓扑和波前层，不能证明配置等价；改用 `resolve-conf` 的 `effective_pipeline` 比对 | `alg_pipeline_tool resolve-conf` 的有效配置输出 |

## 11. 完成后的效果

| 场景 | 现在 | 之后 |
| --- | --- | --- |
| 新增一个自定义 Node | 写函数和 Spec，再登记 CMake（或使用 `--add-to-cmake`） | 写函数和 Spec |
| Node 签名写错 | Batch 和 LLM 钩子报出模板深层错误 | 一句提示期望签名的错误 |
| 新增业务契约（复用已有转换器） | BizDefinition、Exposure、IoBinding 三个结构；批次上限写 4 处，端口表写 3 处；还要登记 CMake | 一个只列端口的 BizDefinition，加一个只写 ID 和批次上限的 IoBinding |
| 新增业务契约（新的载荷语义） | 另写转换器，转换器也要声明批次上限并登记 CMake | 另写转换器的 Decode/Encode，不用声明批次上限，不用改 CMake；业务文件同上一行 |
| 前面请求的结果影响后续请求 | 没有正式支持 | 声明 `state`（含容量上限）；写入节点产出新状态，共享输入端口直接读取，提交边界由框架保证 |
| 用 Studio 编辑配置（阶段 5） | 未修改的数值、布尔、枚举字段被写成显式值 | 只保存改过的字段；Backend 字段默认收起 |
| 新增业务契约（阶段 5） | IoBinding 写 ID 和批次上限；转换器写长度上限常量、容量字段和请求编号端口 | IoBinding 只写 ID；转换器只写业务字段的校验和 Decode/Encode |
| 开启并行执行（阶段 5） | 有未声明 `parallel_safe` 的节点或共享串行模型时，整个配置被拒绝 | 框架把这些节点放进单独的子层顺序执行 |
| 确认单次能送多少条请求（阶段 5） | 分别查池深、`validate-io` 和代码 | `resolve-conf --depth N` 直接给出 |

## 12. 阶段 5：平台与调度参数归位（已完成）

目标：业务开发者（写 Node 函数、转换器 Decode/Encode、编排 Pipeline）不了解平台资源和框架调度，也能接入并运行。
分 5.1–5.5 五步：Studio 保留"未配置"状态；接入层平台数值归位；单次有效批次可查询与清理；请求编号回传移出业务契约；
并行层按约束自动串行。外部契约（Operator 接口、宿主结构体、`.conf`、Pipeline JSON 格式、Demo 与 SDK 的交互）全部不变，
5.5 的兼容放宽已确认并实现：未声明并行安全的节点和共享串行模型的节点自动拆层，原始层的写冲突仍会被拒绝。

默认值的判定原则见 `CONTRIBUTING.md` §3，现行用法见 `tools/pipeline_studio/README.md`、
`doc/dev_guide/business_onboarding.md` 和 `doc/dev_guide/custom_node_concepts.md`。

待方案负责人确认：

- rerank 候选段落上限：Operator 层按 10 MiB 检查，转换器按 64 KiB 拒绝（`src/adapter/input/rerank_input.cpp` 的
  `kMaxCandidatePassageBytes`），实际生效的是 64 KiB，取值未定。
- 配置中显式写出、且等于注册默认值的字段是否删除：基线时 `configs/` 下 161 个、`demo/fixtures/mock/` 下 29 个，配置未修改。
  生成清单的脚本见提交 `12b3345` 中 `doc/platform_parameter_ownership_plan.md` 的附录 A.3。

## 附录：配置变体差异（供方案负责人确认）

各方案不同变体之间的节点、类型和连线完全一致，差异只在节点配置上：

| 方案 | 对比 | 不同的配置 |
| --- | --- | --- |
| doc_qa | cpu 与 default | `TextRuleMatchNode.categories`：default 多出"发票"和 `LOGISTICS_STATUS` 类别；某个类别的关键词在 cpu 中是 "ONNX、llama.cpp"，在 default 中是 "NPU" |
| doc_qa | cpu 与 kite | `TextRuleMatchNode.categories`、`LlmGenerateNode.temperature` |
| doc_qa_rerank | cpu 与 default | `TextRuleMatchNode.default_score/default_category`、`TextTemplateNode.template` |
| doc_qa_rerank | cpu 与 kite | `LlmGenerateNode.temperature` |
| entity_extract | cpu 与 default | `LlmGenerateNode.temperature/max_tokens`、`StructuredJsonParseNode.failure_policy/fallback_json` |
| dialogue_audit | default 与 kite | `LlmGenerateNode.temperature/max_tokens`、`StructuredJsonParseNode.fallback_json`、`TextTemplateNode.template` |
