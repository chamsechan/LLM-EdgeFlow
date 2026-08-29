# RFC-0015 模型能力与推理运行时解耦整改计划

- **制定日期**：2026-08-29
- **关联 RFC**：`0015-model-capability-backend-decoupling`
- **关联分支**：`feat/model-backend-decoupling-rfc`
- **当前状态**：阶段一基础整改已完成，RFC 实现进行中
- **审查依据**：[RFC-0015 桩实现审查与整改清单](0015-model-capability-backend-decoupling-stub-review-20260828.md)
- **后续实现**：[RFC-0015 后续实现指南](0015-model-capability-backend-decoupling-continuation-guide-20260829.md)
- **阶段 3 验收与整改**：[ONNX Embedding 验收结论与实施设计](0015-stage3-onnx-embedding-acceptance-remediation-20260829.md)

## 1. 整改目标

将当前“新接口、生产桩、旧 Engine 调用链并存”的状态，收敛为按 RFC 阶段推进、每个阶段
均可编译、可测试且不虚报生产能力的实现。

整改完成后必须满足：

1. 生产库不注册任何固定返回成功的 Model/Backend 桩；
2. 测试 fake 只存在于测试目标，不进入生产 Catalog；
3. Pipeline 配置、Validator、物化、Session 和 Node 最终只使用 RFC-0015 新契约；
4. Model 与 Backend 通过中性执行协议解耦；
5. 所有批推理经过唯一 `FixedBatchExecutor`；
6. 加载、推理和 Registry 公共边界满足异常安全和 fail-closed 要求；
7. 多模型物化失败不会向 Session 暴露部分结果。

## 2. 总体实施原则

- 严格保持 Layer 2 → Layer 3 → Layer 4 的依赖方向；
- 不同时铺开五个 Model 的固定返回桩；
- 每次只实现一个可运行的纵向切片；
- 没有真实实现和测试的生产 Model/Backend 不得注册；
- Parser、Validator 和 Pipeline 不理解 backend 私有字段；
- 不在 Core 中注入 `platform`、`chip_type`、`device_id` 或 Execution Provider 配置；
- 不在生产代码中保留 emulator/fallback 成功路径；
- 每一阶段完成后先通过对应测试，再进入下一阶段。

---

## 3. 阶段一：收敛 Neutral Contracts 与 Registry

### 3.1 生产桩清理

- [x] 从生产 `FRAMEWORK_SRCS` 移除当前五个固定返回 Model 桩；
- [x] 从生产 `FRAMEWORK_SRCS` 移除模拟 ONNX Runtime 和 llama.cpp Backend；
- [x] 取消这些桩的 `REGISTER_MODEL_WITH_DEFINITION`；
- [x] 取消这些桩的 `REGISTER_BACKEND_WITH_DEFINITION`；
- [x] 保留 `tests/support/inference/` 下的中性 fake；
- [x] 确保 fake 仅链接到测试 target；
- [x] 验证生产 Catalog 不显示尚不可执行的 Model/Backend。

### 3.2 Neutral contracts 完善

- [x] 保留并统一 `ConfigFieldDefinition`；
- [x] 保留唯一 `TraceableItem<T>` 定义；
- [x] 保证 Blackboard 与 Model capability 使用同一套 inference payload；
- [x] 删除重复 DTO，禁止 capability 内嵌套旧 Engine DTO；
- [x] 补齐 include 迁移测试和类型一致性测试。

### 3.3 Tensor contract 加固

- [x] 未知 `ElementType` 必须返回失败，禁止默认按 1 字节处理；
- [x] 校验 shape 中所有维度；
- [x] 拒绝运行时未解析的 `-1`；
- [x] 校验 element count 和 byte size 乘法溢出；
- [x] 明确 Host buffer 对齐保证；
- [x] 增加安全 typed accessor；
- [x] typed accessor 同时校验 dtype、对齐、元素数和 byte size；
- [x] 创建失败时不向调用者留下部分有效 Tensor；
- [x] 增加错误 dtype、shape、buffer、byte size、对齐和溢出测试。

### 3.4 Registry fail-closed

- [x] ModelDefinition 校验空名称、空 capability、空 Creator；
- [x] BackendDefinition 校验空名称、空 Creator、空 protocol 集合；
- [x] 拒绝重复 Model/Backend 名称；
- [x] 拒绝重复或空 config field；
- [x] 校验 config default 与 kind 一致；
- [x] 校验 minimum 不大于 maximum；
- [x] 校验 enum 无重复且默认值在 enum 内；
- [x] Creator 必须在 Registry 锁外执行；
- [x] Creator 异常必须转为失败和 diagnostic；
- [x] `Find` 返回值副本，不暴露 Registry 内部地址；
- [x] Registry 冲突测试使用独立测试进程，避免污染单例状态。

### 3.5 Catalog snapshot

- [x] `Models()`、`Backends()` 不返回可被并发覆写的静态 vector 引用；
- [x] 使用按值 snapshot 或不可变 snapshot 所有权；
- [x] Catalog 内容直接来自双 Registry；
- [x] 增加并发查询测试；
- [x] 测试清理 API 不得永久破坏 static registration。

### 3.6 阶段一完成条件

- [x] 生产 Catalog 不虚报 Model/Backend；
- [x] neutral contract、Tensor、Registry、Catalog 测试全部通过；
- [x] 构建无新增 warning；
- [x] 旧 Pipeline 和 Node 在尚未迁移时仍保持原有回归通过。

---

## 4. 阶段二：完成 Pipeline 规划与原子物化闭环

### 4.1 Parser 新模型配置

模型配置只接受：

```json
{
  "model_id": "embed_model_v1",
  "capability": "embedding",
  "model_type": "bge_embedding",
  "backend": "test_tensor_backend",
  "model_path": "./models/model.onnx",
  "model_config": {},
  "backend_config": {}
}
```

- [ ] `model_id` 必填、非空、唯一；
- [ ] `capability` 必填、非空；
- [ ] `model_type` 必填、非空；
- [ ] `backend` 必填、非空；
- [ ] `model_path` 必填、非空；
- [ ] `model_config` 缺省归一化为 `{}`，存在时必须是 object；
- [ ] `backend_config` 缺省归一化为 `{}`，存在时必须是 object；
- [ ] 拒绝未知字段；
- [ ] Parser 不查询 Registry、不读取文件、不解析私有 config；
- [ ] 删除旧 `engine_type`、`config` 兼容字段；若需要兼容，先更新 RFC 明确迁移规则。

### 4.2 Validator 双 Registry 校验

- [ ] 在任何加载副作用前检查 ModelRegistry/BackendRegistry 冲突；
- [ ] 查找 ModelDefinition；
- [ ] 查找 BackendDefinition；
- [ ] 校验显式 capability 等于 ModelDefinition.capability；
- [ ] 校验 Model required protocol 被 Backend 支持；
- [ ] 按 ModelDefinition 校验和归一化 `model_config`；
- [ ] 按 BackendDefinition 校验和归一化 `backend_config`；
- [ ] 解析 `model_path`；
- [ ] 校验 Node `bind_model` 引用；
- [ ] 校验 NodeDefinition.model_capability；
- [ ] 计算 effective concurrency；
- [ ] 填充 `ValidatedPipelinePlan.models`；
- [ ] 为每个失败项输出稳定 code、JSON Pointer、message 和 suggestion。

### 4.3 ModelRuntimeFactory 完整校验

- [ ] 查找 ModelDefinition 和 BackendDefinition；
- [ ] 创建 Backend provider；
- [ ] 校验 provider `BackendType()`；
- [ ] 调用 Backend `Load`；
- [ ] 校验 session `BackendType()`；
- [ ] 校验 session protocol 属于 BackendDefinition；
- [ ] 校验 session protocol 满足 ModelDefinition；
- [ ] 校验 session concurrency 不比 Definition 更严格；
- [ ] 校验 BatchPolicy 合法；
- [ ] 创建 Model；
- [ ] 校验 ModelType、Capability 和 Concurrency；
- [ ] 保留底层 diagnostic，并增加 model/backend 上下文；
- [ ] 任一步失败均释放已创建对象。

### 4.4 ModelManager 强原子提交

- [ ] staging 内检查空 ID、空 Model 和重复 ID；
- [ ] 检查与现有新 Model 冲突；
- [ ] 兼容期内检查与 legacy Engine ID 冲突；
- [ ] commit 前完成可能失败的资源准备；
- [ ] commit 异常时保持 ModelManager 完全不变；
- [ ] revision 包含 model type、backend、resolved path、双 normalized config；
- [ ] 校验注册元数据与 Model identity 一致；
- [ ] 增加第二个模型失败、commit 异常、重复 ID 和析构次数测试。

### 4.5 Pipeline Build

- [ ] Pipeline 只消费 `ValidatedPipelinePlan.models`；
- [ ] 不重新解析或归一化原始 JSON；
- [ ] 映射为 `ModelLoadSpec`；
- [ ] 在局部容器创建全部 Model；
- [ ] 全部成功后一次调用 `RegisterBatch`；
- [ ] 任一失败时销毁 staging，Session 不增加模型；
- [ ] 删除 `RuntimeOptions.device_id` 向模型配置注入；
- [ ] 不按 backend 名称分支；
- [ ] 不读取 backend 私有配置字段。

### 4.6 阶段二完成条件

- [ ] 使用 test Model/Backend 可完成 Pipeline Build；
- [ ] RFC 新配置可以 validate、plan 和 build；
- [ ] protocol/capability/schema 错误在加载前失败；
- [ ] `backend_config` 从 plan 到 Backend Load 逐值一致；
- [ ] 第二个 Model 失败时第一个不提交；
- [ ] Pipeline 失败后没有部分 ready 状态。

---

## 5. 阶段三：真实 ONNX Embedding 纵向切片

只在阶段一、二完成后实施。

### 5.1 OnnxRuntimeBackend

- [x] vendor 头文件仅位于 `src/engine/backends/onnxruntime/`；
- [x] 真实加载 ONNX 文件；
- [x] 真实读取 I/O metadata；
- [x] 验证 input name、dtype、rank、shape、batch、buffer 和 byte size；
- [x] 真实执行 Session Run；
- [x] 验证全部输出；
- [x] 捕获 vendor 异常；
- [x] model path 不存在或格式错误时失败；
- [x] 未编译 ONNX Runtime 时不注册 `onnxruntime`；
- [x] Backend 不包含 tokenizer、pooling、normalize 或模型类型分支。

### 5.2 BgeEmbeddingModel

- [x] 加载 Definition 声明的 tokenizer sidecar；
- [x] 实现 tokenize、truncate、padding；
- [x] 构造 Tensor Graph 输入；
- [x] 调用 `FixedBatchExecutor::Execute`；
- [x] 调用 `ITensorGraphSession::Run`；
- [x] 实现 output 选择、pooling、normalize；
- [x] 验证输出维度；
- [x] 保持 provenance；
- [x] 失败时清空全部输出；
- [x] 捕获全部预后处理异常。

### 5.3 TextEmbeddingNode

- [x] 改用 `ModelBoundNode<IEmbeddingModel>`；
- [x] 调用 `model()->Embed`；
- [x] 把 normalize 作为 `EmbeddingOptions` 传入 Model；
- [x] Node 不再自行 normalize；
- [x] 保留现有端口、缓存、lifetime 和 revision 语义；
- [x] NodeDefinition 不改变 capability、bind field 和 typed ports。

### 5.4 阶段三完成条件

- [x] Embedding Node 和 DAG 契约不变；
- [x] ONNX 加载与 Run 只存在于 Backend；
- [x] Model 语义只存在于 BgeEmbeddingModel；
- [x] 真实/条件 ONNX 测试通过；
- [x] 不存在 production fallback；
- [x] 配置 validate、plan、build、smoke 全部通过。

---

## 6. 后续迁移顺序

在 Embedding 纵向切片验收后按以下顺序继续：

1. [ ] BGE Reranker 复用同一个 OnnxRuntimeBackend；
2. [ ] LlamaCppBackend + QwenCausalLmModel + LlmGenerateNode；
3. [ ] PPOCR Model + OcrDetectNode；
4. [ ] Paraformer ASR Model + AsrTranscribeNode；
5. [ ] 删除生产 Mock Engine；
6. [ ] 删除组合 OnnxEmbeddingEngine/OnnxRerankEngine；
7. [ ] 删除旧 LlamaCppEngine；
8. [ ] 删除 `IModelEngine`、EngineFactory、EngineDefinition；
9. [ ] 所有配置删除 `engine_type`；
10. [ ] 更新 Catalog、CLI、Studio、README 和 architecture。

---

## 7. 通用异常安全要求

- [ ] typed Model inference 方法检查 null output；
- [ ] 进入方法时清空 output；
- [ ] 空输入返回成功和空 output；
- [ ] 任一批失败时回滚全部 output；
- [ ] 所有 `noexcept` 边界捕获异常；
- [ ] catch 路径返回稳定错误码；
- [ ] Backend 捕获 vendor 异常并写 diagnostic；
- [ ] Registry 捕获 Creator 异常；
- [ ] Layer 1 保持现有 `noexcept` + catch-all；
- [ ] 日志不记录完整 prompt、音频、图片内容或敏感配置。

## 8. FixedBatchExecutor 验收项

- [ ] null output；
- [ ] 空输入；
- [ ] `max_batch_size == 0`；
- [ ] 非法 fixed/max 关系；
- [ ] 小于 max batch；
- [ ] 等于 max batch；
- [ ] 大于 max batch；
- [ ] 非整倍数尾批；
- [ ] 固定 Batch 的 execution count；
- [ ] 动态 Batch 的 execution count；
- [ ] callback 返回数量过少；
- [ ] callback 返回数量过多；
- [ ] callback 返回错误；
- [ ] callback 抛异常；
- [ ] 第二批失败时第一批结果回滚；
- [ ] provenance 顺序和内容保持；
- [ ] 五类 Model 最终全部调用唯一 Executor。

## 9. 测试整改

- [ ] 删除“不存在生产模型文件仍成功 Load”的测试预期；
- [ ] 删除五类生产 Model 固定结果成功测试；
- [ ] Model 单测绑定 test backend/session；
- [ ] test backend 不使用真实平台名称；
- [ ] test backend 不进入生产 Catalog；
- [ ] 增加生产 Catalog 排除 fake/mock 测试；
- [ ] 增加 vendor 未启用时 Backend 不注册测试；
- [ ] 增加 Parser 七字段完整边界测试；
- [ ] 增加 Validator code/path/suggestion 测试；
- [ ] 增加 RuntimeFactory 身份、协议、并发不一致测试；
- [ ] 增加原子物化、析构和资源生命周期测试；
- [ ] 增加 Node typed Model capability 回归测试；
- [ ] 增加全部 Pipeline validate/plan 测试。

## 10. 质量门禁

每个阶段结束均执行：

```bash
./scripts/format.sh
cmake -B build -G Ninja -DLLM_EDGEFLOW_USE_CCACHE=ON
cmake --build build -j$(nproc)
ctest --test-dir build -j$(nproc) --output-on-failure
./scripts/run_all_tests.sh
./build/alg_demo
```

另外必须：

- [ ] 构建无新增 warning；
- [ ] 全部 CTest 100% 通过；
- [ ] 六阶段回归全部通过；
- [ ] 所有配置完成 `validate` 和 `plan`；
- [ ] 至少执行一次 ASan/LSan；
- [ ] 无泄漏、UAF、double free；
- [ ] RFC 和索引在实现期间标记为 `In Implementation`；
- [ ] 只有全部验收通过后才标记为 `Completed`。

## 11. 最终完成标准

- [ ] Node 只依赖 typed Model capability；
- [ ] Model 不 include 具体 Backend 或 vendor 头文件；
- [ ] Backend 不 include Node、Pipeline、AlgContext 或 Blackboard；
- [ ] 生产代码不存在 Model × Backend 组合注册；
- [ ] Embedding 与 Rerank 复用唯一 OnnxRuntimeBackend；
- [ ] LlamaCppBackend 不包含 chat、sampling 或业务回答；
- [ ] OCR/ASR Node 不复制旧 Engine 嵌套 DTO；
- [ ] 所有批推理经过唯一 FixedBatchExecutor；
- [ ] Core 不读取平台、芯片、设备或 Provider 字段；
- [ ] backend_config 仅由对应 Definition 校验、对应 Backend 解释；
- [ ] 模型物化失败不产生部分注册；
- [ ] 生产 Catalog 不包含 test/mock Backend；
- [ ] 生产 Backend 不包含 emulator/fallback；
- [ ] 配置中不再使用 `engine_type`；
- [ ] 文档、Catalog、Studio 和实际运行能力一致。
