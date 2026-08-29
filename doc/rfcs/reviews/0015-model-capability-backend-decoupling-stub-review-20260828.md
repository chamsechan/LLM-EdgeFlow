# RFC-0015 桩实现审查与整改清单（2026-08-28）

## 1. 审查结论

**结论：当前桩实现不建议继续作为可合入的 RFC-0015 实现推进，需先整改。**

当前改动完成了一部分 neutral contracts、Model/Backend 接口、Registry、Catalog 和
`FixedBatchExecutor` 骨架，但存在以下根本问题：

1. 将返回固定结果的 Model/Backend 桩作为生产实现注册进了 `alg_sdk`；
2. Pipeline Parser、Validator、Build 和五个 Node 仍走旧 `engine_type` / `IModelEngine`
   路径，新旧体系没有形成可执行闭环；
3. 多个 `noexcept` 边界内部仍可能抛异常并触发 `std::terminate`；
4. Tensor、Batch、RuntimeFactory 和原子注册契约尚未满足 RFC 中的 fail-closed 要求；
5. 新增测试主要验证了被 RFC 禁止的“无模型文件也返回成功”的桩行为。

因此，当前状态既不是 RFC 第 16 节定义的“阶段 1”完整交付，也不是“阶段 2”可运行的
纵向切片。推荐先收敛为严格的阶段 1，再逐阶段实现。

---

## 2. 审查范围与验证结果

审查基准：

- `doc/rfcs/0015-model-capability-backend-decoupling.md`
- `.agents/skills/llm-edgeflow-developer-guide/references/layer2-pipeline.md`
- `.agents/skills/llm-edgeflow-developer-guide/references/layer3-node.md`
- `.agents/skills/llm-edgeflow-developer-guide/references/layer4-engine.md`
- 当前分支 `feat/model-backend-decoupling-rfc` 的未提交工作区改动

已执行验证：

| 验证项 | 结果 | 说明 |
| :--- | :--- | :--- |
| `git diff --check` | 通过 | 未发现空白符错误 |
| `cmake --build build -j4` | 通过 | 使用 `CCACHE_TEMPDIR=/private/tmp/llm-edgeflow-ccache-tmp` 绕过沙箱缓存目录限制 |
| `ModelBackendDecouplingTest` | 1/1 通过 | 但测试语义包含 RFC 禁止的生产桩成功路径 |
| 完整 CTest | 75/79 通过 | 4 个失败；当前证据不足以认定由本次改动引入，但全量门禁尚未通过 |

完整 CTest 失败项：

1. `OperatorApiTest`：macOS `/var` 与 `/private/var` 路径别名断言失败；
2. `LayerGuardTest`：受限环境中 ccache 临时文件创建失败；
3. `DiagramAssetsCheckTest`：本机 `sha256sum` 兼容性/校验失败；
4. `DiagramRenderGateSelfTest`：图表渲染门禁自测失败。

编译还产生了一个与本次改动直接相关的警告：`DiagnosticCode` 新增枚举没有在
`ValidationCodeToPipelineCode` 中完整处理。

---

## 3. 阻断问题（P0）

### P0-01：生产库注册了 RFC 明令禁止的桩 Model/Backend

证据：

- `CMakeLists.txt:90-96` 将五个 Model 和两个 Backend 无条件加入 `FRAMEWORK_SRCS`；
- `src/engine/backends/onnxruntime/onnxruntime_backend.cpp:26-43` 忽略输入并返回固定 Tensor；
- `src/engine/backends/onnxruntime/onnxruntime_backend.cpp:71-89` 不加载文件却注册
  `onnxruntime`；
- `src/engine/backends/llama_cpp/llama_cpp_backend.cpp:11-69` 使用模拟 codec/logits；
- `src/engine/backends/llama_cpp/llama_cpp_backend.cpp:95-124` 不加载 GGUF 却注册
  `llama_cpp`；
- 五个 Model 的推理方法均直接构造固定结果，例如
  `bge_embedding_model.cpp:72-98`、`qwen_causal_lm_model.cpp:74-93`、
  `ppocr_model.cpp:59-78`。

这直接违反 RFC 第 4.1、6.5、10.6、14.1、16 节：不得提交空 Model、空 Backend、
`not implemented` 注册项或生产 emulator/fallback。

影响：

- `alg_pipeline_tool catalog` 当前会宣称生产支持 5 个 Model 和 2 个 Backend；
- 不存在的 `/tmp/*.onnx`、`/tmp/*.gguf` 仍可“加载成功”；
- 上层无法区分真实推理与伪造结果，属于生产语义错误。

必须修改：

1. 推荐先按“阶段 1”收敛：从生产 `FRAMEWORK_SRCS` 移除全部具体桩 Model/Backend，
   并取消它们的生产注册；
2. 中性 fake 只能保留在 `tests/support/inference/`，且只能链接到测试目标；
3. 生产 Backend 只有在对应第三方运行时可用且真实 `Load/Run` 已实现时才注册；
4. 模型路径不存在、格式错误或 vendor Load 失败必须返回失败；
5. 生产 Catalog 不得显示尚不可执行的 Model/Backend。

验收证据：

- 关闭 ONNX Runtime/llama.cpp 构建选项时，生产 Catalog 中不存在对应 Backend；
- 不存在的模型文件加载失败；
- 测试 fake 不出现在生产 Catalog；
- 仓库中不存在固定业务文本、固定 embedding、固定 OCR/ASR 成功输出的生产路径。

### P0-02：新配置与物化链路没有接通

证据：

- `src/core/pipeline_config.cpp:172-275` 仍只接受
  `model_id + engine_type + model_path + config`；RFC 新增的 `capability`、
  `model_type`、`backend`、`model_config`、`backend_config` 会被判定为未知字段；
- `src/core/pipeline_validator.cpp:443-467` 仍只查询 `EngineFactory` 和
  `EngineDefinition`；
- `include/core/pipeline_validator.h:101-142` 虽声明 `ValidatedModelPlan`，但 Validator
  从未填充 `plan.models`；
- `src/core/pipeline.cpp:238-365` 仍直接创建 `IModelEngine`、注入 `device_id` 并逐个注册，
  没有调用 `ModelRuntimeFactory`，也没有 staging + atomic commit。

影响：

- RFC 示例配置无法解析；
- capability/protocol/双 config schema 校验没有发生；
- `ValidatedModelPlan` 只是死类型；
- `backend_config` 的逐值不变传递无法成立；
- 第二个模型加载失败时，第一个旧模型已经注册，违反原子物化要求；
- Core 仍按 RFC 禁止的方式注入公共 `device_id`。

必须修改：

1. 阶段 1 分支中不要伪装已经具备 Pipeline 物化能力；
2. 阶段 2 一次完成以下纵向闭环：
   - Parser 严格解析 RFC 七字段；
   - Validator 查询双 Registry、校验 capability/protocol/双 schema，并填充
     `ValidatedPipelinePlan.models`；
   - Pipeline 只消费 `ValidatedModelPlan`，不得二次读取/归一化原始 JSON；
   - Pipeline 映射 `ModelLoadSpec`、调用 `ModelRuntimeFactory`、局部 staging 全部模型，
     最后一次 `RegisterBatch`；
   - 删除 Core 的 `device_id` 注入和 backend 名称分支；
3. 若确需兼容期，必须先在 RFC 中明确兼容窗口、解析优先级、冲突规则和删除里程碑；
   当前 RFC 并未授权 `ParsedModelConfig` 同时保留两套字段。

### P0-03：Layer 3 Node 仍依赖旧 Engine capability

证据：

- `src/common_nodes/text_embedding_node.cpp:8-17` 仍依赖 `IEmbeddingEngine`；
- `src/common_nodes/text_rerank_node.cpp:8-17,53-115` 仍依赖 `IRerankEngine::PairInput`；
- `src/common_nodes/llm_generate_node.cpp:4-40` 仍依赖 `ILlmEngine`；
- ASR/OCR Node 同样仍依赖旧 Engine DTO；
- `include/nodes/model_bound_node.h` 只新增了 `model()`，同时保留 `engine()` 兼容别名，
  没有完成实际能力迁移；
- `CMakeLists.txt:97-104` 仍把生产 Mock Engine、组合 ONNX Engine 和旧 llama.cpp Engine
  编译进主库。

影响：

新 Model 即使被 `ModelManager` 注册，现有 Node 也无法 typed cast 到旧 Engine 接口。
Catalog 显示的新 Model/Backend 与实际 Pipeline/Node 执行体系相互隔离。

必须修改：

- 若当前交付定位为阶段 1：不得生产注册具体 Model/Backend，旧 Node 暂时保留是可接受的；
- 从阶段 3 开始按 RFC 顺序逐个做真实纵切片，先完成
  `ONNX Runtime Backend + BGE Embedding Model + TextEmbeddingNode + 配置 + 测试`；
- 每个 Node 改用对应 typed Model capability；禁止只改模板参数命名、继续调用 `engine()`；
- 在阶段 7 完成前保留旧接口时，必须确保生产 Catalog 不同时宣称两套相互不可用的能力。

---

## 4. 高优先级问题（P1）

### P1-01：多个 `noexcept` 边界可能直接 `std::terminate`

典型证据：

- `OnnxRuntimeBackend::Load` 在 `noexcept` 内执行 JSON 访问和 `make_shared`，无 catch；
- `LlamaCppBackend::Load`、`CreateSequence`、`Evaluate` 同样在 `noexcept` 内分配内存；
- 五个 Model 的 `Create` 在 `noexcept` 内执行 JSON 转换、字符串赋值和 `make_shared`；
- 五个 typed inference 方法在 `noexcept` 内 `reserve`、分配 vector/string、
  `emplace_back`，无 catch；
- Registry 的 `Find`/`Has` 标记为 `noexcept`，但加锁和返回 Definition 值副本仍可能抛出。

影响：

内存分配失败、JSON 类型/范围转换异常或系统锁异常不会被转换为稳定错误码，而会终止进程，
违反 RFC 第 5、6.5、7.3、13.2 节。

必须修改：

1. 所有保留为 `noexcept` 的公共边界都需 catch-all，并保证 catch 路径本身尽量不分配；
2. typed inference 失败时清空全部输出，不暴露部分结果；
3. `Load`/Creator 保留底层 diagnostic 并添加 model/backend 上下文；
4. 对无法合理提供不抛保证的查询接口，重新评估是否应标记 `noexcept`，并与 RFC 同步；
5. 增加可控异常注入测试：Creator、JSON 读取、callback、分配/预后处理异常。

### P1-02：`FixedBatchExecutor` 没有严格执行 RFC 输出数量契约，且五个 Model 未调用它

证据：

- `include/engine/fixed_batch_executor.h:80-85` 只拒绝“少返回”，没有拒绝“多返回”；
- 动态 Batch 应要求 `batch_outputs.size() == valid_count`；
- 固定 Batch 应要求 `batch_outputs.size() == execution_count`；
- 五个 Model 虽 include `fixed_batch_executor.h`，但实际没有任何
  `FixedBatchExecutor::Execute` 调用；
- 现有测试只覆盖动态批次和第二批失败，没有覆盖 RFC 第 11.3、17.4 节的完整边界。

必须修改：

1. 将数量检查改为严格相等；
2. 增加 null output、非法 policy、固定尾批、多返回、少返回、callback 抛异常、
   provenance、首批成功后回滚等测试；
3. 每个真实 Model 的批推理路径都必须调用唯一 Executor；
4. 使用无溢出的批次数/循环计算方式，避免 `total + batch_size - 1` 溢出。

### P1-03：Tensor contract 未满足 dtype、对齐、byte size 和 binding 校验要求

证据：

- `include/engine/inference_definition.h:89-101` 对未知 `ElementType` 静默返回 1 字节；
- `HostTensorBuffer` 使用 `std::vector<uint8_t>`，没有向 typed access 证明 float/int 对齐；
- 没有 RFC 要求的 typed data access helper；
- `CreateHostTensor` 只做部分 shape/溢出检查；
- `OnnxRuntimeSession::Run` 完全忽略 input name、dtype、rank、shape、batch、buffer 和 byte
  size，并返回与声明 output spec rank 不一致的 Tensor。

必须修改：

1. 未知 dtype 必须 fail closed；
2. 提供经过 dtype、对齐、元素数和 byte size 验证的 typed accessor；
3. 明确并实现 Host buffer 的对齐保证；
4. `Run` 在调用 vendor API 前验证完整输入 binding，在返回 Model 前验证完整输出；
5. 增加零维、零长度维度、动态维度、溢出、错误 buffer size、错误 dtype/rank/name、
   未知/缺失 binding 测试。

### P1-04：`ModelRuntimeFactory` 校验不完整

`src/engine/runtime/model_runtime_factory.cpp:11-98` 当前只校验 session protocol、ModelType
和 Capability，缺少：

- BackendDefinition 查找；
- `backend->BackendType()` 与请求类型一致性；
- `session->BackendType()` 与 BackendDefinition 一致性；
- session protocol 属于 BackendDefinition.supported_protocols；
- session concurrency 不得比 BackendDefinition 声明更严格；
- Model concurrency 与 ModelDefinition 一致；
- BatchPolicy 合法性；
- 完整底层 diagnostic 的上下文增强。

必须按 RFC 第 9.2、13.1、17.3 节补齐，并对每个不一致项增加独立测试。

### P1-05：`ModelManager::RegisterBatch` 不具备强原子性，并允许跨旧/新表 ID 冲突

证据：

- `include/core/session_context.h:67-69` 只检查 `models_`，没有检查
  `legacy_engines_`；
- `include/core/session_context.h:72-83` 依次写入三张 map；任意一次分配抛异常都会留下
  部分提交；
- 默认 revision 使用指针地址，不满足 RFC 要求的稳定输入组合；
- 注册元数据没有核对 `model_type/capability` 与 Model identity。

必须修改：

1. 新旧存储在兼容期内共享唯一 ID 冲突域；
2. commit 前完成所有可能失败的分配，或使用单一 staging 容器 + swap/回滚机制提供强异常保证；
3. revision 至少包含 RFC 指定的 model/backend/path/双 normalized config；
4. 增加跨旧/新表冲突、commit 中异常、第二模型失败、析构次数测试。

### P1-06：新增测试把错误行为固化为“成功”

证据：

- `tests/test_model_backend_decoupling.cpp:127-143` 期望不存在的 ONNX 文件物化成功；
- `tests/test_model_backend_decoupling.cpp:210-257` 直接断言五类生产桩固定输出成功；
- `tests/test_model_backend_decoupling.cpp:311-322` 虽创建 test backend，但 Model 语义测试
  并未真正使用 test backend fixture；
- 测试没有覆盖 Parser/Validator/Pipeline 新配置闭环、原子物化、生产 Catalog 排除 fake、
  vendor 不可用时不注册等核心验收项。

必须修改：

1. 删除所有“生产桩固定成功”的断言；
2. Model 单测只绑定 `tests/support/inference/` 的受控 fake session；
3. 生产 Backend 测试必须使用真实、最小合法 artifact，或在未启用 vendor 时断言不注册；
4. 增加 RFC 第 17 节列出的 contract、registry、validator、runtime、batch、model、backend、
   node 和业务回归测试；
5. Registry 冲突测试应隔离到独立进程/测试目标，避免永久污染单例冲突状态。

### P1-07：诊断码只声明/字符串化，没有完整映射和产生路径

证据：

- `include/core/pipeline_validator.h:34-39` 新增 6 个 code；
- `src/core/pipeline_validator.cpp` 只补了部分名称映射，尚未产生对应校验；
- `src/core/pipeline.cpp:22-72` 没有完整处理新增枚举，编译器已报告 `-Wswitch` 警告。

必须修改：

- 为每个 code 实现稳定触发路径、准确 JSON Pointer、message、suggestion；
- 完整更新 `DiagnosticCodeName`、PipelineErrorCode 映射、JSON 输出和测试；
- 新增编译代码不得引入 warning。

---

## 5. 中优先级问题（P2）

### P2-01：Catalog snapshot API 存在线程安全和引用生命周期风险

`src/core/pipeline_catalog.cpp:340-351` 每次调用都会覆写函数静态 `cached` vector，然后返回
`const&`。调用者离开 Catalog 锁后，另一个线程可再次赋值同一 vector，造成数据竞争，并使
已有引用的内容在无同步情况下变化。

建议改为按值返回 Registry snapshot，或使用不可变 snapshot 所有权方案。测试应覆盖并发
Catalog 查询。`PipelineCatalog::ClearForTesting` 还会清空静态注册项，而 static registrar
不会自动重放，需限制可见性或改为可恢复的测试隔离机制。

### P2-02：Registry 对 Definition 的 fail-closed 校验不足

当前 Registry 只检查名称、Creator 和 config field 名重复。仍会接受：

- 空 Backend protocol 集合；
- 无效/重复 protocol；
- config default 与 kind 不匹配；
- minimum > maximum；
- enum 重复或默认值不在 enum；
- required 字段携带不一致默认值等非法 schema。

应复用统一 Definition/Config schema 校验逻辑，禁止 ModelRegistry、BackendRegistry 和旧
Catalog 各自实现不同规则，并补充全部非法 Definition 测试。

### P2-03：RFC 生命周期状态应同步为 `In Implementation`

当前已经开始代码和测试实现，但 RFC 文档及 `doc/rfcs/README.md` 仍标记 `Proposed`。
建议在实现分支同步改为 `In Implementation`；只有全部验收和全量门禁通过后才能改为
`Completed`。

---

## 6. 推荐整改顺序

### 第一步：收敛到 RFC 阶段 1

- 保留 neutral contracts、接口、Definition、Registry、Catalog 骨架；
- 移除生产 Model/Backend 桩及其注册；
- fake 仅保留在测试 target；
- 修复 Tensor 基础契约、Registry fail-closed、Catalog snapshot 和异常安全；
- 完成 contract/registry/catalog 测试；
- 确保生产 Catalog 不虚报能力。

### 第二步：完成阶段 2 的单一纵向闭环

- 七字段 Parser；
- 双 Registry Validator + `ValidatedModelPlan`；
- `ModelRuntimeFactory` 完整身份/协议/并发校验；
- Pipeline staging + `RegisterBatch` 强原子提交；
- 使用 test Model/Backend 完成 Build 成功、失败回滚和生命周期测试。

### 第三步：只实现一个真实最小生产切片

- 真实 `OnnxRuntimeBackend`；
- 真实 `BgeEmbeddingModel`；
- `TextEmbeddingNode` 改用 `IEmbeddingModel`；
- 配置迁移、条件注册、真实/条件测试；
- 所有批推理经过 `FixedBatchExecutor`。

该切片完全通过后，再依 RFC 阶段 4～6 依次迁移 Rerank、LLM、OCR、ASR。不要同时铺开
五个 Model 的固定返回桩。

### 第四步：最终收口

- 删除旧 `IModelEngine`、EngineFactory、组合 Engine 和生产 Mock Engine；
- 全部配置删除 `engine_type`；
- 更新 README、architecture、Catalog/Studio；
- 执行格式化、完整 CTest、六阶段脚本、Demo、所有配置 validate/plan、ASan/LSan；
- 全部通过后再把 RFC 和索引标为 `Completed`。

---

## 7. 整改完成检查表

- [ ] 生产库没有固定返回成功的 Model/Backend 桩
- [ ] vendor 未启用时对应生产 Backend 不注册
- [ ] 新七字段配置可被 Parser 严格解析，旧 `engine_type` 按 RFC 计划移除
- [ ] Validator 在任何加载副作用前完成双 Registry、capability、protocol、双 schema 校验
- [ ] `ValidatedPipelinePlan.models` 是 Pipeline Build 的唯一模型计划来源
- [ ] Pipeline 不读取 backend 私有字段，不注入 platform/device 配置
- [ ] 多模型物化失败时 ModelManager 保持完全不变
- [ ] 五个 Node 使用 typed Model capability，不再依赖旧 Engine DTO
- [ ] 所有公共 `noexcept` 边界不会因内部异常终止进程
- [ ] Tensor name/dtype/rank/shape/buffer/byte size/alignment 全部 fail closed
- [ ] Batch callback 输出数量严格等于 execution count，所有 Model 使用唯一 Executor
- [ ] RuntimeFactory 完整校验 Backend/Session/Model identity、protocol、concurrency、policy
- [ ] 测试 fake 只在测试 target，生产 Catalog 不含 fake/mock
- [ ] 新增测试不依赖不存在文件却期待生产 Load 成功
- [ ] 构建无新增 warning
- [ ] `ctest --test-dir build --output-on-failure` 100% 通过
- [ ] `./scripts/run_all_tests.sh` 六阶段全部通过
- [ ] `./build/alg_demo` 通过
- [ ] 所有配置 `validate` 与 `plan` 通过
- [ ] ASan/LSan 无泄漏、UAF、double free
