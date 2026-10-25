# Tensor Graph 文本生成 Model：剩余工作

状态：通用 BPE 分词器、离线生成工具和测试已合入；`tensor_causal_lm` Model 与端到端配置尚未实现。
KV 性能前置验证没有在本计划中确认为完成。恢复实施时以当前主线复核接口及资产。
本计划中的 Model 和配置均为待实现能力，不能当作当前 Catalog 内容。

分词器现行格式与生成方式见 [BPE 文件格式](../doc/dev_guide/bpe_tokenizer_format.md)及
[分词器资产](../configs/README.md#分词器资产onnx-llm)。这里不再复制已交付的分词器设计。

## 范围

- 新增实现 `ILlmModel` 的 `tensor_causal_lm`，类别为 `llm`，使用现有 `tensor_graph` 协议。
  Model 负责 chat 模板、分词、KV cache 与自回归生成，不新增 Backend。
- 端到端目标为 Qwen2.5-0.5B-Instruct 的 int8 ONNX 导出；Gemma3-270M 的端到端运行只作尝试。
- 保持现有 llama.cpp、Kite 和 Qwen 生成行为；共享 ChatML 时要求输出逐字节不变。
- 不新增 fp16 支持、不升级固定的 ONNX Runtime 1.17.3，不将内部 SDK 或硬件兼容性列为已验证。

## 性能前置条件

在实施 Model 前，用临时原型通过 `OnnxRuntimeBackend` 加载目标 Qwen int8 图，
以固定 token 分别预填充到 KV 长度 128、1024、2048，再连续解码 32 步。
记录中性 Session 的总耗时、`Ort::Session::Run` 耗时、两者之差和峰值 RSS；差值包括输入包装、
输出分配、清零和复制。原型与测量数据留在变更说明中，不加入生产目录。

KV 长度 2048 时，封装开销占单步总耗时不得超过 20%。超过则暂停 Model 实施，重新评审执行方式。
第一步需要零长度 past：核对后端的零字节 Tensor 校验，并实际证明 ORT 接受该输入。

## Model 契约

请求状态放在各自的 decoder 中，分词器只读，按 `kConcurrent` 验证并发隔离。
Session 必须使用非固定 batch；逐条请求执行，每次图调用的 batch 为 1。

| 参数 | 类型与约束 |
| --- | --- |
| `tokenizer_file` | 必填 string，声明 `.File()`，读取已有 `.eftok` 格式 |
| `chat_template` | 必填枚举：`chatml` 或 `gemma` |
| `context_size` | 默认 2048，范围 16～1048576，限制 prompt 与生成 token 总数 |
| `decode_batch_size` | 默认 64，范围 1～1048576，且不大于 `context_size`，用于预填充分块 |

参数使用现有 Definition / Parameters 预检，文件路径遵循接入层规则。
ChatML 复用 Qwen 的当前行为。Gemma 模板与 HF `apply_chat_template` 的样例对照：
system 作为第一轮 user 的前缀，content 去首尾空白；分别覆盖有、无 system。

创建时检查图签名：

- `input_ids`、`attention_mask` 为二维 int64；`position_ids` 可选，也为二维 int64。
- KV 输入为成对的 `past_key_values.{i}.key` / `.value`，层号从 0 连续；
  形状为 `[batch, kv_heads, past, head_dim]`，float32，heads 和 head_dim 为静态正数。
- `logits` 为三维 float32；每个 past 有对应 `present.{i}.key` / `.value`，类型和固定维度相符。
- 拒绝其他输入，诊断列明张量名及不符的类型或维度。

decoder 复用通用生成器。编码不额外添加 BOS，逐 token 解码调用 `AppendTokenBytes`。
每块有 n 个 token，构造 `[1, n]` 的 input IDs、全 1 的 `[1, past+n]` mask；
图需要 position IDs 时填入 `past … past+n-1`。首块使用零长度 past，后续接管上一块的 present。

每次执行都检查所有输出的 batch、元素类型和存储大小，present 形状必须为
`[1, kv_heads, past+n, head_dim]`，logits 为 `[1, n, V]`，全部通过后才读取最后位置的 logits。
采样范围为 `min(V, tokenizer.VocabSize())`，所有结束 token 都须在范围内。
静态 V 在创建时检查；动态 V 在每个请求第一次执行时检查，且同一请求后续执行不得改变 V。
范围、形状、上下文错误保留具体原因，失败不发布部分批次结果。

## 实施与验收

按 [CONTRIBUTING](../CONTRIBUTING.md) 分阶段交付，先完成性能前置验证，再实现 Model，最后做端到端验收。

Model 阶段需要以下证据：

- 合法与非法图签名、可选 position IDs、动态词表、越界结束 token、错误输出形状及上下文超限。
- 独立合成状态图让 key、value、mask 和 position 都影响预期 token；检查连续解码、
  `decode_batch_size` 为 1、3、64 时结果一致，以及同实例先后请求和并发请求隔离。
- 采样、结束 token、stop words、ChatML 行为保持和 Gemma 模板样例；ORT 实际执行零字节 past。
- Python ONNX Runtime 1.17.3 对同一 int8 文件、单线程和贪心解码生成独立参考：
  C++ token 序列完全一致，每一步前五名 logits 的绝对误差不超过 `1e-3`。
  真实模型测试缺资产时明确跳过，不能当作通过。
- Catalog 登记、原生 validate/plan、完整门禁、相关 sanitizer 测试与独立 Model/所有权审查。

端到端阶段：

1. 从现有文档问答方案派生 ONNX LLM 配置，接入资产清单与 `--onnx-llm` 下载入口。
   配置中的模型类别为 `llm`，Backend 为 `onnxruntime`，模型参数选择对应分词器和模板。
2. 用 Demo 实际运行该配置，检查非空且可解析的结构化结果；选择检查工具应给出 `verified`。
3. 尝试 Gemma fp32 导出在固定 ORT 上运行；遇到不支持的算子则记录具体限制，不升级 ORT 或宣称已支持。
4. 将已实现参数、使用方式和验证边界写入对应指南，删除本计划及索引。

KV 复制成本、分词器与模型配对、目标导出算子的支持仍须实际验证。
中间非法 UTF-8 的修复属于共享生成器的独立行为变更；本计划不扩大该范围。
