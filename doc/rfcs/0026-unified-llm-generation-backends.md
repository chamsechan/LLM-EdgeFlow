# RFC 0026: 统一 LLM 文本生成协议与多 Backend 实现

- **RFC 编号**：0026-unified-llm-generation-backends
- **创建日期**：2026-09-02
- **文档状态**：Completed
- **关联分支**：`feat/unified-llm-generation-backends`
- **目标版本**：v9.0.0
- **负责人 / 作者**：LLM-EdgeFlow Team

---

## 1. 背景与动机 (Motivation & Context)

当前 `qwen_causal_lm` Model 依赖低层 `causal_lm` 协议，并自行实现 tokenize、增量
logits、采样、EOS、stop words 和 token decode。llama.cpp 提供低层 token/KV 原语，
ONNX Runtime 提供中性 TensorGraph，kiteLLM 则托管完整文本生成。若把这些能力强行塑造成
同一种低层 decoder，会迫使托管引擎伪造能力；若在每个 Backend 复制采样循环，会产生语义
漂移；若 Model 按 Backend 名称分支，则破坏既有 Model/Backend 隔离。

本 RFC 将 Model 所依赖的 LLM 边界提升为统一 `text_generation`：托管引擎直接实现完整
生成；确实提供增量 logits/KV 的 Backend 在内部适配 decoder，并复用一个公共自回归生成器。
首期由 llama.cpp 和条件 kiteLLM 实现该协议；ONNX Runtime 保持现有 `tensor_graph` 能力，
不以无 KV-cache 的 CI 测试桩冒充生产生成能力。

目标是让同一 `qwen_causal_lm`、`LlmGenerateNode` 和 DAG 只替换 Backend、模型路径和该
Backend 的强类型配置，而不修改 Model 或增加 kite 专属上层分支。

## 2. 设计范围与边界 (Scope & Non-Goals)

### 2.1 范围内 (In-Scope)

- [x] 新增 `text_generation` 与 `ITextGenerationSession`；
- [x] 新增仅在 Layer 4 内部使用的 `IAutoregressiveDecoder` 和
  `CommonAutoregressiveGenerator`，不注册到 Pipeline/Catalog；
- [x] llama.cpp 以私有 decoder 复用公共生成器，kiteLLM 直接实现统一会话；
- [x] Qwen 保留 ChatML、system prompt、stop words、UTF-8、固定 seed 和 provenance，
  移除 Model 内部的采样循环；
- [x] `GenerateOptions` 和 `LlmGenerateNode` 增加 `top_k`、`repetition_penalty`，默认值
  保持既有行为；
- [x] kiteLLM 使用显式 SDK 发现、条件编译和条件注册；缺 SDK 时不注册；
- [x] ONNX Runtime 继续只注册 `tensor_graph`，对 `text_generation` fail-closed；
- [x] 各 Backend 继续通过自己的 `BackendDefinition` 暴露强类型配置字段、默认值和范围；
- [x] kite 仅额外暴露 SDK 原生 `run_config_file` 字段，由其自身安全解析和加载。

仅修改 Layer 3 的通用生成选项与 Layer 4 的协议、Model、Backend。Pipeline 文档仍使用
既有 `backend_config` object，Layer 2 的 `PipelineValidator` 仍是唯一校验和规划实现。

### 2.2 非目标 (Non-Goals / Out-of-Scope)

- 不实现流式、异步、取消/deadline、连续批处理、多模态生成或跨请求 prefix cache；
- 不保证跨 Backend 逐 token 一致，只保证参数含义、范围和错误行为一致；
- 不承诺 Backend 使用相同模型文件或 tokenizer 打包格式；
- 不把 decoder 暴露为 Pipeline/Catalog 协议，不在上层加入 kite 专属分支；
- 不引入通用模型包 Manifest、`config_file`/`overrides` 外壳或第二套配置 schema；
- 不实现 ONNX 文本生成、专有 tokenizer 或完整前缀逐 token 重算。ONNX 生成只有在具备
  生产 tokenizer 契约和可用 KV-cache/增量图后，才应由后续 RFC 单独引入；
- 不下载、捆绑或镜像 kiteLLM SDK、模型权重或第三方二进制。

## 3. 总体技术方案与架构设计 (Architecture & Technical Design)

### 3.1 分层映射与不变量

- **Layer 1**：无修改；六个 `Alg_*` 异常屏障保持不变；
- **Layer 2**：无算法修改；Validator 按注册 Definition 校验协议、配置和并发组合；
- **Layer 3**：`LlmGenerateNode` 只增加两个通用采样字段，仍只依赖 `ILlmModel`；
- **Layer 4**：Qwen 只依赖统一会话；具体 Backend 隔离 vendor 类型和资源。

依赖方向固定为 Layer 3 → typed Model → `ITextGenerationSession` → concrete Backend。
内部 decoder/生成器不得引用 Model、Node、Pipeline、Blackboard 或 vendor 类型。

### 3.2 统一协议

`ExecutionProtocol` 使用 `kTextGeneration`，Catalog 名称为 `text_generation`。旧
`kCausalLm`、`ITokenCodec`、`ICausalLmSequence`、`ICausalLmSession` 在本次 pre-release
迁移中删除，避免长期保留两个等价协议。

```cpp
class ITextGenerationSession : public IBackendSession {
 public:
  virtual int Generate(const std::string& formatted_prompt,
                       bool add_bos,
                       const GenerateOptions& options,
                       std::optional<uint64_t> seed,
                       std::string* output,
                       std::string* diagnostic = nullptr) noexcept = 0;
};
```

Model 负责 role/chat template，Backend 接收已格式化 prompt。`seed == nullopt` 表示不要求
固定随机种子；不支持固定 seed 的托管 Backend 必须明确拒绝。空输出指针、非法参数、
tokenize/context/logits 或 vendor 错误均失败并清空输出；异常转换成 diagnostic。

### 3.3 公共自回归生成器

低层 Backend 可实现请求私有 decoder：

```cpp
class IAutoregressiveDecoder {
 public:
  virtual int Encode(const std::string&, bool, std::vector<int32_t>*,
                     std::string*) noexcept = 0;
  virtual int DecodeToken(int32_t, std::string*, std::string*) noexcept = 0;
  virtual bool IsEndToken(int32_t) const noexcept = 0;
  virtual size_t MaxContextTokens() const noexcept = 0;
  virtual int Evaluate(const std::vector<int32_t>& incremental_tokens,
                       std::vector<float>* logits,
                       std::string*) noexcept = 0;
};
```

第一次 `Evaluate` 接收完整 prompt，之后只接收新 token。每步统一执行：检查有限 logits；
repetition penalty；greedy 或 top-k → temperature softmax → top-p → 采样；EOS 不解码；追加
token piece；最早 stop word 截断；最终剥离不完整 UTF-8 后缀。

统一范围为 `max_tokens >= 1`、`temperature ∈ [0,2]`、`top_k >= 0`、
`top_p ∈ (0,1]`、`repetition_penalty ∈ (0,100]`，stop word 不得为空。

### 3.4 Qwen Model 与 Node

`qwen_causal_lm` Definition 要求 `text_generation`。Model 保留 `qwen_chatml`、system
prompt、`add_bos`、`random_seed`，并使用 `FixedBatchExecutor::Execute` 保证 rollback 和
`(req_id, sub_id)` provenance。它只格式化输入、混合固定 seed 和 provenance，再调用
session，不包含 Backend 名称、tokenizer 或采样器分支。

Node 新增 `top_k`（integer `[0, 2147483647]`，默认 0）和
`repetition_penalty`（number `(0,100]`，默认 1.0）。默认值不改变既有 top-p 路径。

### 3.5 Backend 实现

#### llama.cpp

原 codec/sequence 收敛为 Backend 私有 decoder。每次 Generate 创建独立 llama context，
decoder 写入 KV cache，公开 session 串行调用公共生成器。它声明 `text_generation`、
serialized、`BatchPolicy{1,0}`；`llama.h` 只存在于 concrete Backend 源文件。

#### ONNX Runtime

ONNX Runtime 仍只声明 `tensor_graph`，现有 embedding/rerank 等路径不变。首期不注册
`text_generation`，也不增加 tokenizer、特殊 token 或 LLM 图私有契约。Validator 会在规划
阶段拒绝 Qwen + ONNX 组合；直接 Load 的协议不匹配同样在创建 vendor 资源前失败。

#### kiteLLM

CMake 默认 `ENABLE_KITELLM=OFF`。开启时 `KITELLM_ROOT` 必须同时提供版本化 C bridge
`kitellm_edgeflow_adapter.h` 和实现库；配置阶段编译/链接探测 ABI，成功才定义
`HAVE_KITELLM` 并注册，否则 fail-fast，且不自动下载。

Backend 用 RAII 管理 handle/output，一个 handle 用 mutex 串行。统一参数映射到 bridge
同名字段；非空固定 seed 明确拒绝。Catalog 只声明一个可选 string `run_config_file`；该路径
必须相对模型路径、不得穿越或逃逸，存在时由 bridge 实现使用
`kiteLLM_Parameter_SetRunConfigFile`。设备选择同样归该文件所有，Pipeline 若另传显式
`ExecutionTarget` 则拒绝，避免静默忽略或产生双重事实源。

bridge ABI v2 的必需 C 形状如下；第二个字符串是已安全解析的 run-config 路径，可为空：

```c
#define KITELLM_EDGEFLOW_ADAPTER_ABI_VERSION 2
typedef struct kitellm_edgeflow_handle kitellm_edgeflow_handle;
typedef struct {
  size_t struct_size;
  int max_tokens;
  float temperature;
  int top_k;
  float top_p;
  float repetition_penalty;
  const char* const* stop_words;
  const size_t* stop_word_sizes;
  size_t stop_word_count;
} kitellm_edgeflow_generate_options;
typedef struct {
  size_t struct_size;
  const char* data;
  size_t size;
} kitellm_edgeflow_result;
int kitellm_edgeflow_create(const char* model_path,
                            const char* run_config_file,
                            kitellm_edgeflow_handle**);
void kitellm_edgeflow_destroy(kitellm_edgeflow_handle*);
int kitellm_edgeflow_generate(kitellm_edgeflow_handle*, const char*, size_t,
                              int, const kitellm_edgeflow_generate_options*,
                              kitellm_edgeflow_result*);
void kitellm_edgeflow_result_release(kitellm_edgeflow_result*);
const char* kitellm_edgeflow_last_error(kitellm_edgeflow_handle*);
```

### 3.6 配置、生命周期与迁移

`BackendDefinition::config_fields` 是 Pipeline 可配置字段的唯一 schema，Validator 在规划期
检查类型、范围和拼写并注入默认值。Backend 在 Load 边界可基于同一字段集合做防御性检查，
但不引入通用 JSON 解包器：

- ONNX Runtime：保留已有 `max_batch_size`、线程数和图优化等级；
- llama.cpp：保留 `context_size`、`decode_batch_size`、线程数、GPU layers 和 tensor check；
- kiteLLM：仅 `run_config_file`。

直接 artifact 路径和现有扁平 `backend_config` 保持不变。模型包 Manifest、公共
`config_file`/`overrides` 会把强类型 Catalog 降级成不透明 JSON，并让拼写错误延迟到
加载期，因此不采用。

Backend Session 是 session-scoped 共享资源，Qwen 持有 session；decoder、tokens、logits、
RNG 和输出为请求私有。llama.cpp/kite 声明 serialized；Model 声明 concurrent，Planner
取更严格值。所有 public `noexcept` 捕获标准和未知异常，失败清空输出；vendor 类型不离开
concrete Backend。托管输出复制进 `std::string` 后由 RAII 释放。

Pipeline 的 Node、端口、DAG、capability、model type 和 bind_model 不变。协议由
Definition/Factory 自动选择，不写入持久 JSON。

## 4. 关键设计考量与权衡 (Design Trade-offs & Invariants)

1. Model 不读取 Backend 名称或私有配置；Backend 不处理 ChatML；
2. 统一边界是完整文本生成，托管/低层差异只存在于 Backend 内部；
3. 只有具备生产增量 decoder 的 Backend 才复用公共采样器；不为协议闭环制造低效实现；
4. vendor 头、类型与资源只在 concrete Backend；公共头只含中性类型；
5. Catalog/Validator/Planner 只消费注册 Definition，不维护手工组合表；
6. `FixedBatchExecutor::Execute` 继续统一 rollback 与 provenance；
7. 参数语义一致不等于跨 Backend 确定性一致；
8. kite SDK 必须由版本化 bridge 证明，缺 SDK 时不注册并 fail-closed；
9. 默认 `top_k=0`、`repetition_penalty=1.0` 保持现有行为；
10. 新 Backend 通常只需实现一个统一会话和一个强类型 Definition，不扩张 Core schema。

## 5. 测试与质量验收计划 (Testing & Verification Plan)

- [x] 公共生成器：greedy、top-k、top-p、temperature、repetition penalty、seed、EOS、
  stop words、context、空/NaN/Inf logits、codec 失败、rollback、UTF-8；
- [x] Qwen：只接受 `text_generation`、ChatML/add_bos、seed/provenance、切片与 rollback；
- [x] llama.cpp：强类型 Definition、decoder 会话、错误行为和可选真实 GGUF；
- [x] ONNX：现有 TensorGraph 行为保留，Qwen 组合和直接文本生成请求均被拒绝；
- [x] kite：无 SDK 不注册；有 bridge 时覆盖条件构建、run-config、参数映射、RAII、seed
  拒绝和串行声明；真实模型由环境变量启用条件专项生成；
- [x] Catalog/Validator/Planner：同一 Qwen 可与每个已注册 text-generation Backend 组合，
  未声明字段在规划期拒绝；
- [x] Node Definition/Init：新增字段默认值、范围和传递；
- [x] 对最终提交集只运行一次新的 `LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh`。

验收环境提供脚本锁定 SHA-256 的 Qwen2.5 0.5B GGUF，但不要求仓库捆绑 kite SDK 或模型。
真实 llama.cpp 文本生成必须运行；kite 真实模型测试保留条件门禁，不能把 SDK 缺失下的跳过
描述为真实生成成功。可控 bridge fixture 用于证明 SDK 发现、ABI、参数映射和资源释放。

## 6. 实施路线与里程碑 (Implementation Milestones)

1. [x] RFC、接口/生命周期/SDK/测试矩阵自检；
2. [x] 统一协议、公共生成器、Qwen 和 Node；
3. [x] llama.cpp 与条件 kiteLLM；ONNX 保持 TensorGraph；
4. [x] 移除 ONNX 生成测试桩、模型包和通用配置外壳，恢复强类型校验；
5. [x] 聚焦验证、架构图、Changelog、新的一次完整门禁与 `Completed` 收口。

只有范围内实现和必需门禁全部通过后，RFC 才能更新为 `Completed`。

## 7. 变更记录 (Changelog)

| 日期 | 版本 | 变更内容 | 作者 |
| :--- | :--- | :--- | :--- |
| 2026-09-02 | v1.0.0 | 建立统一文本生成协议与三 Backend 设计 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.1.0 | 完成接口闭环、分层不变量、生命周期、SDK 发现与测试可行性自检，进入实施 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.2.0 | 完成首版三 Backend 实现与门禁 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.3.0 | 根据部署评审重新进入实施，尝试模型包与私有配置文件方案 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.4.0 | 完成模型包与通用部署外壳试验 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.5.0 | 根据生产可用性复审重新进入实施：移除无 KV-cache 的 ONNX 生成与未落地模型包/通用 overrides，恢复强类型规划期校验 | LLM-EdgeFlow Team |
| 2026-09-02 | v1.6.0 | 完成精简实现、真实 GGUF、条件 kite bridge、强类型规划及 85/85 完整门禁，状态更新为 Completed | LLM-EdgeFlow Team |
