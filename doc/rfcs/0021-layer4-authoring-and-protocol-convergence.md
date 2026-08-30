# RFC 0021: Layer 4 作者体验与执行协议收敛

- **RFC 编号**：0021-layer4-authoring-and-protocol-convergence
- **创建日期**：2026-08-30
- **文档状态**：Completed
- **关联分支**：`refactor/layer4-runtime-convergence`
- **目标版本**：v5.6.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

RFC-0015 已完成 Model 语义与 Backend 运行时解耦，但当前 Layer 4 在继续接入 Model、
Backend 和模型变体时仍有几处扩展摩擦：

- `ICausalLmSession::Evaluate` 接受任意 `ISequenceState`，sequence 所属 Session 和模型资源
  生命周期没有由类型系统绑定；
- `BackendDefinition` 可声明多个协议，但 `BackendLoadSpec` 不携带本次请求的协议；
- ONNX `BatchPolicy` 只根据第一个输入推导，不能正确表达其他输入或输出中的静态 batch；
- Embedding 与 Reranker 重复实现 BERT 输入契约，且构造期验证强度不一致；
- BGE Model 将 Backend Session 并发能力作为自身并发能力返回，与 Definition 和 Qwen
  的语义不一致；
- `FixedBatchExecutor` 同时保留新旧两套入口，Model 作者面对不一致的异常和输出数量契约。

本 RFC 在不重新引入组合式 `*Engine`、不改变 Pipeline JSON 和业务 Node 行为的前提下，
收敛这些 Layer 4 契约，使后续实现更容易复用公共机制，并让扩展失败尽量发生在加载或
构造阶段。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] 用行为型 `ICausalLmSequence` 取代可与任意 Session 配对的纯状态句柄。
- [x] llama.cpp sequence 持有模型资源和串行执行锁，允许其安全独立于 Session 析构。
- [x] `BackendLoadSpec` 携带可选 `requested_protocol`，Factory 在加载前显式传递模型要求。
- [x] ONNX 从全部输入和输出 metadata 推导唯一合法的 `BatchPolicy`。
- [x] 抽取 BERT 输入 metadata 契约，Embedding 与 Reranker 共用同一构造期验证。
- [x] 收敛 BGE 输出的公共 dtype、rank、batch 契约，各 Model 仅保留自身 layout 语义。
- [x] BERT 输入对象仅在 Session 声明时分配和发布可选 `token_type_ids`。
- [x] BGE Model 并发声明只描述模型语义可重入性，Backend 并发继续独立参与有效并发计算。
- [x] 删除无生产调用的旧 `FixedBatchExecutor` 重载，保留唯一 `BatchPolicy + BatchSlice`
  入口。
- [x] Embedding 输出拒绝 NaN/Inf，并补齐相关回归测试。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不修改六个 `Alg_*` C ABI、Operator、Adapter 或 Blackboard 契约。
- 不修改持久 Pipeline JSON Schema，也不删除现有 Model/Backend 配置字段。
- 不在本期引入通用 `StatusOr`、流式生成、取消/Deadline 或动态 Backend 插件 ABI。
- 不合并 Model 与 Backend，不让厂商类型离开具体 Backend 实现目录。
- 不在缺少基准数据时实现 ORT 输出零拷贝、llama context pool 或采样器算法替换。
- 不为了减少行数引入通用 `BaseModel`、反射框架或深继承体系。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 架构分层映射 (4-Tier Mapping)

- **Layer 1**：无修改。
- **Layer 2**：无行为修改；继续根据 Model/Backend Definition 计算有效并发。
- **Layer 3**：无接口与行为修改；仍调用现有能力 Model。
- **Layer 4**：收敛中性 Causal LM sequence、Backend LoadSpec、Tensor/batch 合约和公共
  Model 支持代码。

依赖方向保持 Layer 1 → Layer 2 → Layer 3 → Layer 4。Layer 4 不引用 Pipeline、Node、
Blackboard 或业务配置。

### 3.2 Causal LM sequence

```cpp
class ICausalLmSequence {
 public:
  virtual ~ICausalLmSequence() = default;
  virtual int Evaluate(const std::vector<int32_t>& tokens,
                       std::vector<float>* logits,
                       std::string* diagnostic = nullptr) noexcept = 0;
};

class ICausalLmSession : public IBackendSession {
 public:
  virtual ITokenCodec& TokenCodec() noexcept = 0;
  virtual size_t MaxContextTokens() const noexcept = 0;
  virtual std::unique_ptr<ICausalLmSequence> CreateSequence(
      std::string* diagnostic = nullptr) noexcept = 0;
};
```

每个 sequence 封装自己的执行状态和 `Evaluate` 行为。具体 Backend 负责让 sequence 持有
其所依赖的共享模型资源与锁，不再由调用者把 Session 与裸状态句柄配对。

### 3.3 Backend requested protocol

`BackendLoadSpec` 增加可选 `requested_protocol`。`ModelRuntimeFactory` 在静态 Definition
匹配成功后设置该字段。单协议 Backend 若收到不支持的显式协议必须在加载厂商资源前拒绝；
未设置字段的直接调用保持兼容。

### 3.4 Tensor 与 Batch 契约

ONNX Backend 扫描所有输入和输出第一维：

- `0` 为非法 batch；负数表示动态；
- 全部动态时使用配置的动态 `max_batch_size`；
- 任一静态维度出现时，所有静态 batch 必须相等，并生成 fixed policy；
- 冲突在创建 `Ort::Session` 后、返回公开 Session 前明确失败。

BERT 公共支持层验证 `input_ids`、`attention_mask` 和可选 `token_type_ids` 的名称、dtype、
rank、sequence 和 batch metadata，并复用输出的 dtype、rank、正维度和 batch 校验。
Embedding 与 Reranker 只保留各自 output layout、pooling 或 activation 语义。

### 3.5 单一批处理入口

生产代码继续使用：

```cpp
FixedBatchExecutor::Execute(inputs, policy, run_batch, outputs);
```

删除旧的 `fixed_max_batch + dummy_pad_input + std::function` 重载。Dummy 内容仍由 Model
callback 根据 `BatchSlice::execution_count` 构造，Executor 只负责切片、数量验证、padding
结果剥离、provenance 和失败回滚。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. **组合优于继承**：BERT 复用收敛到窄函数/值对象，不创建包含 Embedding/Reranker
   分支的基类。
2. **所有权显式**：sequence 直接拥有执行行为与依赖资源，避免跨 Session 状态误配。
3. **兼容加载**：`requested_protocol` 为可选字段，保留现有直接 Backend 测试与调用方式。
4. **并发分离**：Model Definition 描述语义对象可重入性，Backend Definition/Session
   描述运行时能力，有效并发仍取更严格者。
5. **错误尽早暴露**：metadata、协议和 batch 冲突在模型构造或 Backend Load 时失败，
   不延迟到首个业务请求。
6. **不推测性能收益**：本期只消除错误抽象和重复路径，零拷贝、资源池和采样器优化需
   独立 benchmark 证据。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] `test_qwen_causal_lm_model.cpp` 覆盖 sequence 独立状态、失败回滚和生成语义。
- [x] `test_llama_cpp_backend.cpp` 使用 sequence 自身执行，并保留真实 GGUF 可选门禁。
- [x] ONNX 测试覆盖全动态、其他端口静态、静态冲突和零 batch metadata。
- [x] Embedding/Reranker 测试共享 BERT 输入构造期契约，并覆盖 NaN/Inf。
- [x] ModelRuntimeFactory 测试证明 `requested_protocol` 在 Backend Load 前可见。
- [x] BatchExecutor 测试全部迁移到唯一入口并保持 padding/provenance/rollback 证据。
- [x] 交付前运行一次 `./scripts/run_all_tests.sh`。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] **阶段一：RFC 建立并索引**。
2. [x] **阶段二：Causal LM 和 Backend Load 协议收敛**。
3. [x] **阶段三：Tensor、BERT 与 Batch 公共机制收敛**。
4. [x] **阶段四：聚焦测试与完整质量门禁**。
5. [x] **阶段五：RFC 状态更新为 Completed**。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-08-30 | v1.0.0 | 建立 Layer 4 作者体验与协议收敛方案 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.1.0 | 完成协议、契约、并发语义与批执行器实现及聚焦验证 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.2.0 | 完整门禁 85/85 通过，RFC 状态更新为 Completed | LLM-EdgeFlow Team |
| 2026-08-30 | v1.3.0 | 合并 BGE 输出公共校验，减少 Model 重复实现并保持原诊断契约 | LLM-EdgeFlow Team |
| 2026-08-30 | v1.4.0 | 修复 BGE 测试进程并行时的临时目录碰撞 | LLM-EdgeFlow Team |
