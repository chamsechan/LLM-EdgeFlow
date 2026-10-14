# BPE 分词器与 Tensor Graph 文本生成 Model：设计与实施计划

状态：设计已确定，尚未实施。

- 分词器按**算法**组织：只写一个通用的 BPE 实现，各模型分词流程的差别用文件头参数表达，不按模型拆分模块。
- 文本生成新增一个 Model，跑在现有的 `tensor_graph` 协议上，**不新增 Backend**。

本计划描述的能力，在对应 PR 合入前都不成立。

基线：`main` 提交 `e40dc262`。本地项目 `gemma-270m-tokenizer`（提交 `dd4afd6`）只作参考，用到它的
Unicode 表生成、NFC、Qwen 预分词、文件映射和对比 HF 的验证方式；SentencePiece 按分数合并的实现不沿用。
实施时以届时的生产代码、[架构契约](../doc/architecture.md)、[开发者指南](../doc/developer_guide.md)
和 [Model Execution 契约](../.agents/skills/llm-edgeflow-developer-guide/references/model-execution.md) 为准。
开发与交付流程沿用 [CONTRIBUTING](../CONTRIBUTING.md)。全部阶段合入后，把形成的规则写进现行指南，删除本计划。

## 1. 目标与范围

### 1.1 目标

1. 在 `src/engine/text/` 实现一个通用的 BPE 分词器 `text::BpeTokenizer`。它读取离线生成的紧凑文件，
   与 HF `tokenizers` 的 BPE 流程逐 token 一致。
2. 提供离线工具 `tools/prepare_bpe_tokenizer.py`：为每个模型单独生成一个紧凑文件。工具读取该模型的 HF `tokenizer.json`，
   根据其中的组件类型确定参数；对于由 SentencePiece 转换来的词表（如 Gemma），还会读取 `tokenizer.model` 中的分数。
   支持的组件组合见 4.1；遇到不支持的组合，明确报错。
3. 新增 Model `tensor_causal_lm`（类别 `llm`，所需协议 `tensor_graph`），负责 chat 模板、分词、KV cache 和自回归循环。
   每一步通过现有的 `ITensorGraphSession::Run` 执行 ONNX 解码图。
4. 端到端验收模型为 `Qwen2.5-0.5B-Instruct` 的 int8 ONNX 导出。Gemma3-270M 的分词正确性要验收，端到端只做尝试，原因见 9.1。

### 1.2 不在范围内

- **新增或修改 Backend 的协议和参数。** `onnxruntime` 后端只修一处零字节张量的校验，见 4.6。
- **`llama_cpp`、`kite_llm`、`qwen_causal_lm` 的行为**：保持不变。
- **BPE 以外的算法**（Unigram、WordPiece），以及本次两个模型用不到的 BPE 组件（例如 Llama2 的前置 `▁`、Llama3 的正则和 `ignore_merges`）。
  以后需要时，在对应枚举里加一个值即可。
- **fp16 KV 和 fp16 logits**：中性的 `ElementType` 没有 fp16，ORT 后端在加载时就会拒绝这类图。
- **ORT 升级**：固定的 ORT 1.17.3 不变。
- **生成文本中间非法 UTF-8 的修复**：现有生成器只去掉结尾不完整的 UTF-8，llama.cpp 路径也是如此；要改，应改在共享生成器里。
- **内网 SDK**：只要它实现了 `tensor_graph`，原则上可以直接复用这个 Model；能否运行，要进内网后对照真实接口确认。

## 2. 设计决策

| # | 决策 | 理由 |
|---|---|---|
| D1 | 分词器只有一个 BPE 实现。规范化、预分词、字节回退和解码器这四个环节的差别，以及合并优先级的来源（D2），都作为文件头里的参数；参数由工具从 `tokenizer.json` 读出，与模型名无关 | 两个目标模型在 HF 中都是 `"type": "BPE"`，都是逐对合并相邻片段，差别只在前后处理环节和合并优先级的来源（见 4.1）。这样合并核心、特殊 token 识别和文件读取只需写一份；新模型只要用到的是已支持的组件，就不用改代码 |
| D2 | 合并时统一用“拼接后查词表”找到合成的 token；优先级来源是文件参数 `merge_priority`，二选一：`rank` 表示每个 token 带着自己的合并规则（切分位置和排名）；`score` 表示每个 token 只带一个分数，优先级由“分数、左右两段的字符数、合成 token 的 ID”在运行时组合得出。工具为每个模型挑选适合的一种，并保证候选合并的先后顺序与 HF 的排名完全相同；查找路径和合并循环只写一份 | 代码通用的是这两种算法做法，与具体模型无关；每个模型各有一个文件，其中的合并数据只属于这个模型。`rank` 对任何 BPE 都与 HF 一致；`score` 更省内存，只适用于 SentencePiece 转换来的词表，并且生成时要证明顺序等价（见 4.1）。各方案的数据量见 4.3 |
| D3 | 分词器代码放在 `src/engine/text/`，生成工具放在 `tools/` | 分词器需要 Model 和 Backend 都能引用，而 Node 引用不到。生成工具由开发者手动运行。构建时只调用它的 `fixture` 子命令生成合成文件，这样不必在两处各维护一份文件格式写入代码 |
| D4 | 第三方源文件和生成的分词器文件都不进 git 仓库 | 它们由 `fetch_real_test_models.sh --onnx-llm` 下载或生成，`.gitignore` 覆盖；仓库里只保留 SHA 记录，以及只含文本和 ID 的测试样例 |
| D5 | 文本生成由 Model 负责分词和生成循环，跑在 `tensor_graph` 上 | 分词属于预处理，与 BGE 的做法一致；按“类别 + 协议”选择实现时，`llm` + `tensor_graph` 只对应这一个 Model，与 `qwen_causal_lm`（`text_generation`）不冲突 |
| D6 | Model 参数为 `tokenizer_file`、`chat_template`、`context_size`、`decode_batch_size`；`chat_template` 是必填枚举（`chatml`、`gemma`） | 模板是否和权重匹配直接决定输出质量，属于语义契约，必须显式写明；用枚举是为了不引入模板引擎 |
| D7 | 不提供 `add_bos`、`tokenizer_type` 参数 | 与 HF 一致：Gemma 模板直接输出 `<bos>` 文本，编码时识别为特殊 token；Qwen 不需要 BOS。分词流程由文件头标明 |
| D8 | 结束 token 写进分词器文件 | 它是模型事实（`generation_config.json` 的 `eos_token_id`），不应由部署者配置 |
| D9 | KV 只支持 fp32 | 现有类型系统不支持 fp16 |

## 3. 数据流

```
Node（如 llm_generate） ──IModel::Generate──▶ TensorCausalLmModel（新增）
  ├─ chat_templates：chatml / gemma（src/engine/models/common；qwen_causal_lm 改为共用 chatml）
  ├─ text::BpeTokenizer：Encode / AppendTokenBytes / IsEndToken
  └─ CommonAutoregressiveGenerator（现有）驱动 TensorGraphDecoder（新增，属于 Model）
        └─ ITensorGraphSession::Run（现有 onnxruntime 后端）
             input_ids / attention_mask / [position_ids] / past_key_values.* → logits / present.*

text::BpeTokenizer::Encode：
  识别特殊 token 并切开（在原始文本上） → 规范化 → 预分词 → 初始单位 → 按 merge_priority 合并 → ID
```

## 4. 组件设计

### 4.1 分词流程与参数

下表的事实来自两份 HF `tokenizer.json`，已核实：

| 环节 | 枚举值 | Gemma3-270M | Qwen2.5-0.5B |
|---|---|---|---|
| 特殊 token 识别（added tokens） | 通用，不设参数 | 6,415 个 | 22 个 |
| 规范化 `normalizer` | `none` / `replace_space_metaspace` / `nfc` | `replace_space_metaspace`（空格换成 `▁`） | `nfc` |
| 预分词 `pre_tokenizer` | `none` / `byte_level_qwen2` | `none` | `byte_level_qwen2`：Qwen2 的切分正则，加上 `ByteLevel(use_regex=false)` |
| 初始单位 | 由预分词决定，不单独设参数 | UTF-8 字符 | 字节 |
| 字节回退 `byte_fallback` | 开 / 关 | 开：字符不在词表中时，拆成 `<0xXX>` token | 关：字节级不存在未知字符 |
| 合并优先级 `merge_priority` | `rank` / `score`；都通过拼接后查词表来确定合成的 token，按优先级合并，优先级相同时取最左 | `score`：分数来自 `tokenizer.model` | `rank`：每个 token 最多 1 条合并规则 |
| 解码器 `decoder` | `metaspace_byte_fallback` / `byte_level` | `▁` 换回空格；连续的 `<0xXX>` 拼成字节，再用 HF ByteFallback 的规则处理非法 UTF-8；最后 Fuse | 拼接所有字节，再对整体做有损 UTF-8 解码 |

生成工具会检查 HF 中每个组件的类型和字段。下列情况一律明确报错：

- `dropout` 非空；
- `continuing_subword_prefix` 或 `end_of_word_suffix` 非空；
- `ignore_merges` 为 true；
- 特殊 token 的 `lstrip`、`rstrip`、`single_word` 或 `normalized` 为 true（两个目标模型都是 false）；
- 正则与已知模式不完全相同；
- 环节组合与上表的两种不一致。

Gemma 的 `fuse_unk` 为 true，但同时开启了字节回退，`<unk>` 不会被产生，所以接受。

`score` 只在调用时提供了 `tokenizer.model` 才使用。此时工具检查两项，任一不满足就报错，不会悄悄改用 `rank`：

1. merges 的集合与“两段都在词表中、拼起来也在词表中”的全部切分完全相同；
2. 把 merges 按 **（分数从高到低，左段字符数从多到少，右段字符数从多到少，合成 token 的 ID 从小到大）** 排序后，
   与 HF 的 merges 顺序逐条相同，并且没有并列。

运行时 `score` 模式按同一个组合键比较候选合并（字符数是 Unicode 码点个数，随合并累加），
因此候选合并的先后顺序与 HF 的排名完全相同；同一合并规则出现在多处时，两者都取最左。等价性在生成时被证明，不依赖测试语料。
已核实 Gemma 满足这两项：514,906 条规则零冲突、零并列。只看分数会有 278,657 处并列；用字节数代替字符数会有 465 处冲突。
分数本身不能从 merges 的排名推算出来，所以必须读取 `tokenizer.model`。

没有提供 `tokenizer.model` 时一律用 `rank`；`rank` 对任何 BPE 都成立，是新模型的默认选择。

### 4.2 `text::BpeTokenizer`

新增文件：

```
src/engine/text/bpe_tokenizer.{h,cpp}           # 对外接口；文件映射、头部和 CRC 校验；编码流程；合并核心；特殊 token 识别；解码
src/engine/text/unicode_tables.{h,cpp}          # 读取文件中的 Unicode 区段：NFC 和字符类别查询
src/engine/text/byte_level_pretokenizer.{h,cpp} # byte_level_qwen2 的手写切分（不在运行时用正则引擎）
```

`src/engine/` 下的私有头文件会自动进入 Model 和 Backend 可见的头文件视图，**不要**加进 `capability_nodes` 视图。

```cpp
namespace llm_edgeflow::text {
inline constexpr size_t kMaxEncodeBytes = size_t{1} << 20;
class BpeTokenizer final {
 public:
  static std::shared_ptr<const BpeTokenizer> Load(
      const std::string& file, std::string* diagnostic = nullptr) noexcept;
  // 有效 UTF-8，长度不超过 kMaxEncodeBytes；文本中的特殊 token 识别为对应 ID。
  int Encode(std::string_view text, std::vector<int32_t>* ids,
             std::string* diagnostic = nullptr) const noexcept;
  // 追加单个 token 的原始字节；特殊 token 追加空串。
  int AppendTokenBytes(int32_t id, std::string* bytes,
                       std::string* diagnostic = nullptr) const noexcept;
  // 完整解码：跳过特殊 token，非法 UTF-8 按解码器参数处理，与 HF 一致。
  int Decode(const std::vector<int32_t>& ids, std::string* text,
             std::string* diagnostic = nullptr) const noexcept;
  bool IsEndToken(int32_t id) const noexcept;
  const std::vector<int32_t>& EndTokens() const noexcept;
  size_t VocabSize() const noexcept;  // 最大 ID + 1
};
}
```

实现要求：

- **错误处理**：全部 `noexcept`，返回 `int` 状态码并写诊断；内部用 `catch (const std::exception&)` 和 `catch (...)` 兜底。
  加载完成后实例只读，可以在多个线程间共享。
- **编码**：
  1. 在原始文本上，按“最左、最长”规则识别特殊 token 并切开；
  2. 其余片段依次经过规范化和预分词，再转成初始单位；
  3. 每个片段用优先队列，按 `merge_priority` 给出的优先级合并，同优先级时取最左。
- **规模**：所有偏移和长度使用 `uint32_t` 或 `size_t`，容量计算做溢出检查。`replace_space_metaspace` 会把 1 个字节扩成 3 个，所以规范化后的长度按输入的 3 倍计算。
  工作区按输入长度动态分配，不设固定上限的数组。
- **平台**：使用只读 `mmap`，要求小端，加载时检查。

### 4.3 文件格式

格式说明写在 `doc/dev_guide/bpe_tokenizer_format.md`，随 S1 的代码一起提交。要点如下：

- **文件头**：
  - magic `EFBPE\r\n\0`，**不设版本字段**；
  - 文件长度和 CRC-32。CRC 只用来发现意外损坏，不是认证手段；
  - 4.1 节的五个参数（规范化、预分词、字节回退、合并优先级、解码器）；
  - token 总数、合并规则数、特殊 token 数、结束 token 数；
  - 各数据区的偏移。所有数据区 4 字节对齐，加载时检查边界和枚举取值。
- **数据区**：
  - token 字节池及其偏移；
  - token 类型：普通、字节回退 `<0xXX>`（附带字节值）、特殊；
  - 词表哈希：“token 字节串 → ID”的开放寻址哈希表，合并时用它查拼接结果；
  - 合并优先级，按 `merge_priority` 二选一：
    - `rank`：每个 token 一个偏移，指向它自己的合并规则；每条规则打包在一个 `uint32` 里，高 24 位是排名，低 8 位是左段的字节长度。
      工具检查排名小于 2^24、token 不超过 255 字节，超出就报错；
    - `score`：每个 token 一个 float32 分数；
  - 不另存“解码后的字节”：`metaspace_byte_fallback` 在解码时把 `▁` 换成空格，省下一份与字节池同样大小的数据；
  - 初始单位表：字节级是 256 个单字节 token；UTF-8 字符级是“单字符 → ID”的有序表；
  - 特殊 token 匹配表（字节 trie）；
  - 结束 token 列表；
  - Unicode 区段：只有 `nfc` 或 `byte_level_qwen2` 需要时才写入。

合并数据的大小，按两份真实 `tokenizer.json` 估算（只算合并相关的数据，含词表哈希；S1 实测）：

| 存法 | Gemma3-270M | Qwen2.5-0.5B | 说明 |
|---|---|---|---|
| 显式规则表 `(左, 右) → (排名, 结果)` | 9.9 MiB | 3.7 MiB | 已放弃：Gemma 的规则表大部分是冗余数据 |
| `rank` | 5.0 MiB | 3.2 MiB | 通用，结果与 HF 完全一致 |
| `score` | 3.0 MiB | 不适用 | 只适用于 SentencePiece 转换来的词表 |

加上字节池、偏移等其他数据后，整个文件估计为：Gemma 用 `score` 约 6.5 MiB，用 `rank` 约 8.5 MiB；Qwen 用 `rank` 约 4.9 MiB。

### 4.4 生成工具与资产

新增 `tools/prepare_bpe_tokenizer.py`。它只依赖输入文件，不包含按模型名组织的配置表：

| 子命令 | 作用 | 依赖 |
|---|---|---|
| `compile --tokenizer-json T --generation-config G [--sentencepiece-model S] [--unicode-data U] --output F` | 为一个模型生成分词器文件，结果必须是确定的。提供 `S` 时使用 `score`，否则用 `rank` | 读取 `tokenizer.model` 只解析其中的 piece、score、type 三个 protobuf 字段，用标准库完成，不需要 `sentencepiece` 或 `protobuf` 包。写 Unicode 区段时需要 `tokenizers==0.23.3`（从这个版本的 HF 后端取出正则所用的 Unicode 属性）和 Unicode 9 数据 |
| `fixture {metaspace,byte_level} --output F` | 两种组件组合各生成一个合成小词表，供默认门禁使用：`metaspace` 用 `score`，`byte_level` 用 `rank`，两种优先级都有覆盖 | 只用标准库，不下载 |
| `golden --tokenizer-json T --output J` | 生成“文本 → ID”样例 | `tokenizers==0.23.3` |
| `verify --tokenizer-json T --tokenizer-file F --driver EXE` | 用 C++ 驱动与 HF 比对编码和解码结果 | `tokenizers==0.23.3` |

Unicode 9 数据：`https://www.unicode.org/Public/9.0.0/ucd/UnicodeData.txt`，SHA-256
`68dfc414d28257b9b5d6ddbb8b466c768c00ebdf6cbf7784364a9b6cad55ee8f`。HF 的规范化器使用 Unicode 9。

已核实的资产事实（2026-10-14 查询 HF）：

| 资产 | revision | 文件和 SHA-256 | 已核实的内容 |
|---|---|---|---|
| Qwen/Qwen2.5-0.5B-Instruct | `7ae557604adf67be50417f59c2c2f167def9a775` | `tokenizer.json` `c0382117…7539`；`generation_config.json` `e558847a…ebe6` | 组件见 4.1；`eos_token_id` = `[151645, 151643]` |
| onnx-community/Qwen2.5-0.5B-Instruct | `22942cb7d7ba4cc81bb4673549ca4d4614469b5e` | `onnx/model_int8.onnx`，512,096,557 字节，`41834041…6100` | 输入有 `input_ids`、`attention_mask`、`position_ids`、`past_key_values.{i}.key/value`；输出有 `logits`、`present.*`；只用 `MatMulInteger` 等标准算子；24 层、2 个 KV 头、`head_dim` 64；logits 维度 **151,936** |
| onnx-community/gemma-3-270m-it-ONNX | `2dbbfdb1b59bd034eb959428c6a7da9dd7ea27f0` | `tokenizer.json` `55da1312…aac8`；`tokenizer.model` `1299c11d…e79c`；`generation_config.json` `94ef2242…1632` | 组件见 4.1；**没有 `position_ids` 输入**；KV 为 float32，形状 `[batch, 1, past, 256]`；18 层；logits 维度 262,144；`eos_token_id` = `[1, 106]`；所有变体都用到 `com.microsoft:GroupQueryAttention`，int8 和 q4 变体还用到 `GatherBlockQuantized` |

**资产文件命名与下载**：所有资产平铺在 `configs/` 下：

- 源文件命名为 `<模型>-tokenizer.json`、`<模型>-tokenizer.model`（只有 Gemma 有）、`<模型>-generation_config.json`；Unicode 数据为 `unicode-9.0.0-UnicodeData.txt`；
- 生成的文件命名为 `<模型>.eftok`，表示 EdgeFlow 的分词器文件；具体的算法由文件头的 magic 标明（BPE 为 `EFBPE`）。`.gitignore` 增加 `configs/*.eftok`；
- 以上文件全部由 `.gitignore` 覆盖。

`configs/asset_manifest.json` 中的完整 SHA 分两个阶段加入，下载分组都是 `onnx-llm`：

- **S1**：两个模型的源文件、Unicode 数据，以及生成文件的 `sha256` 和 `"source": "generated by tools/prepare_bpe_tokenizer.py"`。
  同时给 `fetch_real_test_models.sh` 增加 `--onnx-llm` 模式：下载这个分组，调用工具生成文件，再校验 SHA。
  生成用的 Python 解释器由 `EDGEFLOW_TOKENIZER_PYTHON` 指定，默认 `python3`，需要预先安装 `tokenizers==0.23.3`。
- **S3**：Qwen2.5 的 ONNX 模型和 `selections` 中的 `tensor_causal_lm-onnxruntime`。Gemma 的这两项只有在 9.1 节的尝试成功后才加入。

### 4.5 Chat 模板与 `TensorCausalLmModel`

**Chat 模板**：新增 `src/engine/models/common/chat_templates.{h,cpp}`。

| 枚举值 | 输出 | 来源 |
|---|---|---|
| `chatml` | `qwen_causal_lm` 现有的 `ApplyChatTemplate`，原样搬过来 | `qwen_causal_lm` 改为调用共用函数，行为不变 |
| `gemma` | `<bos><start_of_turn>user\n{system}\n\n{prompt 去掉首尾空白}<end_of_turn>\n<start_of_turn>model\n`；没有 system 时省略 `{system}\n\n` | HF 的 Gemma Jinja 模板：system 作为第一轮 user 的前缀，content 会做 `trim` |

**Model**：新增 `src/engine/models/tensor_causal_lm/tensor_causal_lm_model.{h,cpp}` 和 `tensor_graph_decoder.{h,cpp}`。

| 定义项 | 值 |
|---|---|
| 实现名 | `tensor_causal_lm` |
| 接口 | `ILlmModel` |
| 所需协议 | `kTensorGraph` |
| 并发 | `kConcurrent`：每次请求的状态都在局部 decoder 里，分词器只读 |

| 参数 | 类型 | 默认值 / 范围 | 说明 |
|---|---|---|---|
| `tokenizer_file` | string，`.File()`，必填 | — | 工具生成的分词器文件 |
| `chat_template` | 枚举，必填 | `chatml` / `gemma` | 见上 |
| `context_size` | int | 2048 / [16, 1048576] | prompt 与生成 token 的总上限，与 `llama_cpp` 同名同义 |
| `decode_batch_size` | int | 64 / [1, 1048576] | 预填充时每块的 token 数，与 `llama_cpp` 同名同义；`Validate` 要求不大于 `context_size`。ONNX 导出会对每个位置都输出 logits，Qwen 一块约 37 MiB，Gemma 一块约 64 MiB |

**`Create` 检查**（失败时返回诊断，不创建 Model）：

1. 加载分词器。
2. 用 `session->Inputs()` / `Outputs()` 解析签名：
   - **必需的输入**：`input_ids`、`attention_mask`，int64，二维；
   - **可选的输入**：`position_ids`，int64，二维；
   - **KV 输入**：成对的 `past_key_values.{i}.key` / `.value`，`i` 从 0 连续编号；四维 `[batch, kv_heads, past, head_dim]`，
     `kv_heads` 和 `head_dim` 必须是静态正数，类型为 float32；
   - **输出**：`logits`，float32，三维；每个 past 都要有对应的 `present.{i}.key` / `.value`，类型相同，
     第 1 维（kv_heads）和第 3 维（head_dim）相同，第 2 维（序列长度）在运行时检查；
   - **其他输入一律拒绝**，诊断中列出名字。
3. **可采样范围**：`sample_vocab = min(logits 最后一维, tokenizer.VocabSize())`，全部结束 token 都必须小于它。
   - logits 最后一维是**静态**的（两个目标模型都是）：在 `Create` 时检查；
   - 是**动态**的：由每次请求的 decoder 在第一次 `Run` 后检查，结果只记在这个 decoder 里，同一请求后续的 `Run` 要求维度不变。
4. batch 策略沿用 `qwen_causal_lm` 的要求：必须是非固定 batch，逐条请求处理，每次 `Run` 的 batch 为 1。

**`TensorGraphDecoder`**（实现 `IAutoregressiveDecoder`，每次请求新建一个）：

| 方法 | 行为 |
|---|---|
| `Encode` / `DecodeToken` / `IsEndToken` | 分别调用 `tokenizer.Encode` / `AppendTokenBytes` / `IsEndToken`；Model 传入的 `add_bos` 总是 false |
| `MaxContextTokens` | `context_size` |
| `Evaluate(tokens)` | 按 `decode_batch_size` 分块执行。每块：`input_ids` 为 `[1, n]`；`attention_mask` 为 `[1, past + n]`，全为 1；图中有 `position_ids` 时，取值为 `past … past+n-1`。第一块使用长度为 0 的 past，之后把上一块的 `present.*` 移交为 `past_key_values.*`。返回最后一个位置的 logits 中前 `sample_vocab` 个值 |

**每次 `Run` 之后都要检查**，任一项不满足就让本次生成失败：

- 所有输出的 batch 都为 1；
- 每个 `present.*` 的形状为 `[1, kv_heads, past + n, head_dim]`；
- `logits` 的形状为 `[1, n, V]`，`V` 与之前确定的值一致，元素数与形状相符。

只有全部通过，才读取 logits 的最后一行。

### 4.6 对现有代码的小改动

| 文件 | 改动 | 原因 |
|---|---|---|
| [onnxruntime_backend.cpp](../src/engine/backends/onnxruntime/onnxruntime_backend.cpp) `ValidateInputTensor` | `ByteSize() == 0` 时允许 `Data()` 为空指针 | 第一步的 past 长度为 0；`HostTensorBuffer(0)` 本身合法。需确认 ORT 1.17.3 接受这样的张量 |
| [qwen_causal_lm_model.cpp](../src/engine/models/qwen_causal_lm/qwen_causal_lm_model.cpp) | `ApplyChatTemplate` 改为调用共用的 `chatml` 函数 | 共享的 helper 放在公共目录；行为不变，`check_layer_isolation.sh` 对 Qwen 目录的检查不受影响 |
| [common_autoregressive_generator.h](../src/engine/text_generation/common_autoregressive_generator.h) 的注释，以及 [developer_guide.md](../doc/developer_guide.md) | “Backend 私有”改为“供 Backend 或 Model 使用的内部组件，不作为 Pipeline/Catalog 协议” | 新 Model 要复用它 |
| [source_layout.md](../doc/dev_guide/source_layout.md) | `tools/` 一行加入分词器生成工具，说明构建时只调用其 `fixture` 子命令 | D3 |

## 5. 配置

```json
{
  "type": "llm",
  "name": "llm_model",
  "file": "qwen2.5-0.5b-instruct-int8.onnx",
  "params": {
    "tokenizer_file": "qwen2.5-0.5b-instruct.eftok",
    "chat_template": "chatml",
    "context_size": 1024
  },
  "backend": { "type": "onnxruntime" }
}
```

Gemma 只需换文件名，并写 `"chat_template": "gemma"`。后端参数（线程数等）照常写在 `backend.params`。
用 [pipeline-composer](../.agents/skills/pipeline-composer/SKILL.md) 从 [pipeline_doc_qa_cpu.json](../configs/pipeline_doc_qa_cpu.json)
派生 `configs/pipeline_doc_qa_onnx_llm.json`，只替换 `llm_model` 这一条。

## 6. 诊断

| 场景 | 发现阶段 | 预期诊断 |
|---|---|---|
| `tokenizer.json` 含不支持的组件或字段 | 工具 `compile` | 写明组件名和字段 |
| 缺少 `tokenizer_file` 或 `chat_template`，或枚举值非法 | Validator | 现有的字段诊断 |
| `decode_batch_size` 大于 `context_size` | `ParamSpec().Validate` | 两个字段的约束冲突 |
| fp16 等不支持的类型 | ORT 后端 `Load` | 现有的 `unsupported ONNX data type` |
| 分词器文件损坏、magic 不对、参数取值未知 | `Create` 第 1 步 | 写明路径和原因 |
| 签名不符 | `Create` 第 2 步 | 写明具体的名字或维度 |
| 结束 token 超出可采样范围 | logits 维度静态：`Create` 第 3 步；动态：该请求第一次 `Run` 之后 | 写出 ID 和范围 |
| 运行时形状不符 | `Evaluate` | 写明张量名、期望形状和实际形状 |
| prompt 超过上下文 | 生成器 | 现有的 `Prompt exceeds text generation context` |

## 7. 实施阶段

- S1 到 S3 每个阶段使用独立分支，单独验收。上一阶段合入、`main` 的 CI 通过后，再从最新的 `main` 开始下一阶段。
- 本计划随 S1 一起提交。
- S2 涉及 Model 的执行和所有权，交付前需要独立的 Reviewer 审查。

### S0：KV 复制开销原型（不提交，S2 的前置条件）

做法 2 接受了“每一步都复制全部输出”的成本，但并没有消除它，所以先用原型测出实际开销。S0 与 S1 互不依赖，但 S0 不通过就不开始 S2。

- **做法**：写一段临时代码，直接调用 `OnnxRuntimeBackend::Load`（`tensor_graph` 协议）加载 Qwen2.5 int8 模型，本地临时打上 4.6 节的零字节张量补丁，使用固定的 token ID。
  先预填充，使 KV 长度分别达到 **128、1024、2048**；然后在每个长度下连续解码 32 步，把每步的 `present.*` 移交为下一步的 `past_key_values.*`。
- **测量**：每个长度下，`ITensorGraphSession::Run` 的总耗时，以及其中 `Ort::Session::Run` 本身的耗时（在 `Run` 内临时计时）。两者之差就是封装层的全部开销：输入包装、输出缓冲的分配和清零、memcpy。同时记录进程的峰值 RSS。
  按估算，Qwen 每个 token 的 KV 约 24 KiB，长度 2048 时每步要复制约 48 MiB。
- **判据**（预先确定）：长度 2048 时，封装层开销占单步总耗时不超过 20%，就进入 S2；超过就暂停，重新讨论是否改为新增 Backend。
- **产出**：三个长度的数据写进 S2 的 PR 描述，原型代码不提交。Gemma 能否在 ORT 1.17.3 上运行尚不确定（见 9.1），S0 不测 Gemma。

### S1：BPE 分词器与生成工具

- **新增文件**：
  - 4.2 节的源文件，`tools/prepare_bpe_tokenizer.py`，`doc/dev_guide/bpe_tokenizer_format.md`；
  - `tests/unit/engine/test_bpe_tokenizer.cpp`；
  - `tests/support/bpe_tokenizer_driver.cpp`，`verify` 子命令使用的驱动程序，不注册为 CTest 用例；
  - `tests/fixtures/bpe_tokenizer/<模型>.golden.json`：只含测试文本和 ID。
- **修改文件**：
  - `tests/RuntimeTests.cmake`：构建时调用 `fixture` 子命令生成两份合成文件，并添加驱动程序目标；
  - `configs/asset_manifest.json`、`scripts/fetch_real_test_models.sh`、`.gitignore`：见 4.4；
  - `configs/README.md`、`doc/README.md`、`tests/README.md` 的相应说明；
  - `source_layout.md`：见 4.6；
  - `doc/CHANGELOG.md`：新增 BPE 分词器组件和生成工具。
- **默认门禁内的测试**，两种合成文件都要覆盖：
  - **加载失败**：文件不存在、被截断、magic 不对、CRC 不对、数据区越界、枚举参数取值未知；
  - **合并顺序**：两种合并优先级都要测。优先级高的先合并（`rank` 看排名，`score` 看分数）；同一优先级出现在多处时，先合并最左边的；
  - **编码**：
    - 非法 UTF-8 被拒绝；
    - 长度恰好为 `kMaxEncodeBytes` 能编码，多 1 字节被拒绝；
    - 跨 64 KiB 的输入，64 KiB 前后的内容不同，以便发现长度或偏移被截断；
    - 全是空格的输入，测 3 倍膨胀；
    - 特殊 token 被识别，与普通文本相邻时也能正确切开；
  - **解码**，分两组：
    - 只由有效 UTF-8 字节组成的序列：逐个 `AppendTokenBytes` 拼起来，等于 `Decode` 的结果；
    - 含非法字节的序列：`Decode` 按各自解码器的 HF 规则输出 U+FFFD，`AppendTokenBytes` 输出原始字节，两者按设计不相等；
  - **NFC 长输入**：例如 `a` 后接 N 个 U+0301 和 N 个 U+0323，接近 `kMaxEncodeBytes` 时仍在线性对数时间内完成；
  - **其他**：ID 越界报错；特殊 token 解码为空串；`IsEndToken` 与合成文件一致；多个线程同时 `Encode` 同一实例，结果一致；
    每个用例使用自己的临时文件，`ctest -j` 并行运行时互不干扰。
- **工具测试**：
  - `compile` 对每一种不支持的组件或字段都报错（用手工构造的小 `tokenizer.json`）；
  - 提供了 `tokenizer.model`，但 merges 不等于全部切分，或排序与 HF 顺序不一致时，报错。要包含 `abc` 反例：HF 编码为 `ab,c`，而只按分数会得到 `a,bc`；
  - 字节回退 token 只出现在 `added_tokens`、不在 BPE 基础词表中时，报错；
  - `verify` 在驱动缺行、多行或输出 `ERR` 时失败，不会报告零差异；
  - `fixture` 两次输出的文件逐字节相同。
- **真实资产测试**：设置 `EDGEFLOW_TEST_BPE_TOKENIZER_DIR` 时运行，否则 `GTEST_SKIP`。要求与样例 ID 完全一致，并检查词表大小和结束 token。
- **验收**：
  1. 两个模型都用 `verify` 与 HF 比对，编码和解码零差异。语料包括随机 Unicode、最长 64 KiB 的文本、ChatML 和 Gemma 模板文本，以及随机 ID 序列；
  2. 用错的分词器文件去比对，必须报大量差异，证明比对本身有效；
  3. 生成两次，SHA 相同，并与 manifest 一致；记录两个文件的大小，与 4.3 的估算对照（Gemma 用 `score` 约 6.5 MiB，Qwen 约 4.9 MiB）；
  4. 默认门禁 `./scripts/run_all_tests.sh` 通过；
  5. ASan/UBSan 下相关测试通过。`run_sanitizers.sh` 不支持只跑部分用例，按脚本相同的环境设置手动运行。

### S2：Model、chat 模板和框架小改动

- **新增文件**：4.5 节的源文件；`tests/unit/engine/test_tensor_causal_lm_model.cpp`；`tools/reference_onnx_generate.py`。
- **修改文件**：4.6 节列出的文件；`tests/RuntimeTests.cmake`；
  [generate_test_onnx_model.py](../scripts/generate_test_onnx_model.py)，增加两张合成解码图：
  - **采样图**：`logits = Gather(W, input_ids)`，`W[v]` 的最大值落在 `(v+1) mod V`，用于检查采样和结束 token；
  - **状态图**：单层，batch、序列长度和 past 长度都是动态维度。
    - `present.0.key = Concat(past.key, Cast(input_ids))`；
    - `present.0.value = Concat(past.value, Cast(position_ids) + 1)`；
    - `t = (1 × ReduceSum(present.key ⊙ mask) + 3 × ReduceSum(present.value ⊙ mask)) mod V`，`logits = OneHot(t, V)`，形状为 `[batch, n, V]`。

    key 和 value 的权重不同，所以 key 或 value 任一缓存丢失、位置被重置、mask 填错，`t` 都会改变。
    另生成一个不含 `position_ids` 的变体：`present.0.value = Concat(past.value, 2 × Cast(input_ids) + 1)`，用于覆盖 Gemma 的签名形式。
- **默认门禁内的测试**：
  - **签名解析**：合法签名；缺少 `input_ids`；层号不连续；KV 维度不对应；缺少 `present`；出现未知输入；logits 不是 float32；
  - **状态图**：
    - `decode_batch_size` 取 1、3、64 时，结果相同；
    - 连续解码得到的 token 与手算的序列一致；
    - 同一个 Model 先后两次请求、两个线程同时请求，彼此不受影响；
    - 不含 `position_ids` 的变体能正常生成；
  - **出错路径**：输出形状错误时报错；prompt 超过上下文；遇到结束 token 停止；`stop_words` 截断；
  - **可采样范围**：logits 维度静态时，结束 token 越界在 `Create` 时失败；维度动态时，在该请求第一次 `Run` 后失败，同一请求中途维度变化也失败；
  - **chat 模板**：`chatml` 的输出与修改前的 `qwen_causal_lm` 逐字节相同；`gemma` 的输出与 HF `apply_chat_template` 的样例一致，有 system 和无 system 两种情况都要覆盖；
  - **零字节张量**：`ValidateInputTensor` 接受它，并且 ORT 能实际执行。
- **真实模型测试**：设置 `EDGEFLOW_TEST_TENSOR_LLM_DIR` 时运行。
  - **参考结果**：`tools/reference_onnx_generate.py` 用 Python 的 `onnxruntime==1.17.3` 加载**同一个 int8 文件**，
    单线程、贪心解码，独立实现生成循环；记录每一步选中的 token 和 logits 的前 5 名，存为只含数字的 golden 文件；
  - **判据**：C++ 同样用单线程、temperature 0，token 序列与 golden 完全一致；每一步前 5 名 logits 的绝对误差不超过 `1e-3`；
  - **效果评估**（不作为门槛）：用固定的 10 条 prompt，与 HF fp32 的贪心结果对比，把前缀一致的长度写进 PR 描述。
- **验收**：
  1. 以上测试全部通过；
  2. 第 5 节的配置能通过 `alg_pipeline_tool validate` 和 `plan`，Catalog 中 `llm` 类别下出现 `tensor_causal_lm`；
  3. 默认门禁通过；
  4. sanitizer 下相关测试通过；
  5. Reviewer 审查通过。

### S3：端到端配置、文档和收尾

- **新增文件**：`configs/pipeline_doc_qa_onnx_llm.json`，必要时增加 Demo Profile。
- **修改文件**：`configs/asset_manifest.json`，加入推理模型和 selection（见 4.4）；以及第 8 节列出的文档。
- **Gemma 尝试**：用 fp32 的 `model.onnx` 在 ORT 1.17.3 上运行。fp32 版本不使用 `GatherBlockQuantized`，但仍然使用 `GroupQueryAttention`。
  能跑通，就加入 Gemma 的资产和 selection；跑不通，就在 `doc/VERIFIABLE_SELECTION.md` 中记为“未验证：需要更高版本的 ORT”，不升级 ORT。
- **验收**：
  1. 用 `--onnx-llm` 完成下载和生成后，用 `alg_demo` 实际运行新配置，得到非空、可解析的结构化结果；
  2. `tools/verify_selection.py` 对新的 selection 给出 `verified`；
  3. 默认门禁通过；
  4. 删除本计划，并从 `plans/README.md` 中移除它的索引。

## 8. 文档更新清单

| 文档 | 内容 | 阶段 |
|---|---|---|
| `doc/dev_guide/bpe_tokenizer_format.md` | 文件格式 | S1 |
| [configs/README.md](../configs/README.md) | `--onnx-llm` 的步骤和 Python 依赖 | S1 |
| [source_layout.md](../doc/dev_guide/source_layout.md) | 见 4.6 | S1 |
| [doc/developer_guide.md](../doc/developer_guide.md) | 新 Model 的参数；在 `tensor_graph` 上由 Model 负责分词和生成循环；修改关于生成器的那句话 | S2 |
| [Model Execution 参考](../.agents/skills/llm-edgeflow-developer-guide/references/model-execution.md) | 把新 Model 列为参考实现 | S2 |
| [doc/VERIFIABLE_SELECTION.md](../doc/VERIFIABLE_SELECTION.md) | 新的 selection、真实模型的验证方法、Gemma 的验证状态 | S3 |
| [doc/CHANGELOG.md](../doc/CHANGELOG.md) | 每个阶段交付时各写一条：S1 分词器组件和生成工具；S2 新 Model；S3 端到端配置 | S1–S3 |

## 9. 风险与待决事项

### 9.1 Gemma 端到端

onnx-community 的 Gemma 导出，所有变体都使用 `com.microsoft:GroupQueryAttention`，int8 和 q4 变体还使用 `GatherBlockQuantized`。
后者据我所知要比 1.17 更新的 ORT 才支持，这一点尚未核实。fp32 变体（权重约 1.1 GB）能否在 1.17.3 上运行，在 S3 实测。
即使跑不通，Gemma 的分词正确性（S1）和模板正确性（S2）也已分别验证，缺的只是运行这一环。

### 9.2 其他

| 项目 | 处理方式 |
|---|---|
| KV 每一步都要复制 | S0 实测，判据见 S0 |
| `score` 与 HF 是否等价 | 生成时检查 4.1 的两项条件，从而证明顺序等价；`verify` 只作为补充确认。新模型不满足条件时，工具报错，改用 `rank` |
| 分词器与模型配错 | 加载时只能检查 ID 范围，发现不了所有错配；由 manifest 的 selection 保证资产配对 |
| 输出中间的非法 UTF-8 | 与现有 llama.cpp 路径一致；修复属于共享生成器的改动，另行决定 |
| Unicode 数据许可 | 分词器文件不进仓库，在本地生成，因此 `THIRD_PARTY_NOTICES.md` 不变；以后如果要分发生成的文件，需要补上 Unicode 许可 |
