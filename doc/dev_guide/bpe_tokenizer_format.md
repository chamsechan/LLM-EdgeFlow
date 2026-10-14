# BPE 分词器文件格式

`.eftok` 文件由 [prepare_bpe_tokenizer.py](../../tools/prepare_bpe_tokenizer.py) 从模型的 Hugging Face
`tokenizer.json` 等源文件生成，由 `text::BpeTokenizer`（`src/engine/text/`）以只读 `mmap` 方式加载。
同一份输入始终生成逐字节相同的文件。文件命名为 `<模型>.eftok`，不入库；下载、生成和 SHA-256 校验见
[配置说明](../../configs/README.md#分词器资产onnx-llm)。

分词器只有一个通用的 BPE 实现，不按模型拆分。各模型分词流程的差别用文件头的五个参数表达，
参数由工具从 `tokenizer.json` 读出，与模型名无关；格式只覆盖下面列出的组件，不是通用的 Hugging Face 加载器。

## 五个参数

| 参数 | 取值（头部整数） | 含义 |
| --- | --- | --- |
| `normalizer` | `none`(0) / `replace_space_metaspace`(1) / `nfc`(2) | 对每个非特殊片段先做的规范化：不处理 / 把空格换成 `▁`（U+2581） / Unicode NFC |
| `pre_tokenizer` | `none`(0) / `byte_level_qwen2`(1) | 预分词：整个片段是一个词 / 按固定正则切分后转为字节 |
| `byte_fallback` | `0` / `1` | 字符不在词表中时，是否拆成 `<0xXX>` token（仅字符级初始单位有意义） |
| `merge_priority` | `rank`(0) / `score`(1) | 合并优先级的来源，见下 |
| `decoder` | `metaspace_byte_fallback`(0) / `byte_level`(1) | 解码方式，见下 |

初始单位由预分词决定：`none` 时是 UTF-8 字符，`byte_level_qwen2` 时是字节。

没有格式版本字段：magic `EFBPE\r\n\0` 只标识这是 BPE 分词器文件。布局变化时直接在工具和加载器中原地修改，
并重新生成文件、更新 `asset_manifest.json` 中的 SHA-256；发布前没有需要兼容的旧文件。
需要新组件（例如 Llama2 的前置 `▁`、Llama3 的正则、`ignore_merges`）时，在对应枚举里增加取值。

工具读取 `tokenizer.json` 后，下列情况一律明确报错，不会悄悄降级：

- `dropout` 非空；`continuing_subword_prefix` 或 `end_of_word_suffix` 非空；`ignore_merges` 为真；模型类型不是 `BPE`；
  `truncation`、`padding` 非空；
- 特殊 token 的 `lstrip`、`rstrip`、`single_word` 或 `normalized` 为真；
- 规范化器、预分词器、解码器不是已支持的写法，或预分词的正则与已知模式不完全相同；
- 环节组合不是下面两种之一：

| 规范化 | 预分词 | 字节回退 | 解码器 |
| --- | --- | --- | --- |
| `Replace(" ", "▁")` | 无 | 开 | `Replace("▁", " ")` + `ByteFallback` + `Fuse` |
| `NFC` | `Split`（Qwen2 正则，`Isolated`）+ `ByteLevel(use_regex=false)` | 关 | `ByteLevel` |

`fuse_unk` 为真但同时开启了字节回退时不会产生 `<unk>`，所以接受。`post_processor` 与分词器文件无关
（BOS 由 chat 模板文本给出，编码时按特殊 token 识别），工具不读取。

## 通用约定

- 整数为小端 `uint32`，加载器在大端宿主上直接报错。文件由 160 字节头部和 16 个数据区组成，
  每个数据区从 4 字节对齐位置开始，首尾相接，最后一个数据区结束于文件末尾；头部的偏移字与区段一一对应。
  文件不含指针或 C++ 对象，加载器直接读取映射区，不复制词表。
- 头部字 2 为文件总字节数，字 3 为**从第 16 字节到文件末尾**的 CRC-32/IEEE（与 Python `zlib.crc32` 一致），
  因此参数和计数也在校验范围内。
- 加载时检查：magic、文件长度、CRC、枚举取值、各计数、数据区布局和边界、token 偏移单调性、token 类型、哈希表与词表一致、
  合并规则、初始单位、字典树边界、结束 token 范围，以及 Unicode 区段的行格式和排序。
  任何一项不通过都返回空指针，诊断中带文件路径和原因。
- **CRC 只能发现意外损坏，不是认证手段**：它不证明文件来源，也挡不住有意构造的文件。
  来源由生成时对源文件的 SHA-256 校验和 `asset_manifest.json` 记录的 `.eftok` SHA-256 保证。
  部署时只读使用，更新时原子替换，不要截断正在被映射的文件。
- CRC 扫描会访问全部映射页，加载后的内存占用包含整个文件。

## 头部（40 个字）

| 字 | 内容 |
| --- | --- |
| 0–1 | magic `EFBPE\r\n\0` |
| 2 | 文件总字节数 |
| 3 | CRC-32 |
| 4–8 | `normalizer`、`pre_tokenizer`、`byte_fallback`、`merge_priority`、`decoder` |
| 9 | token 总数（ID 为 0 到 总数−1，连续） |
| 10 | 合并规则条数（`score` 时为 0） |
| 11 | 特殊 token 数 |
| 12 | 结束 token 数 |
| 13 | 字节池长度 |
| 14 / 15 | 哈希槽数（2 的幂，大于登记数）/ 哈希登记数 |
| 16 | 初始单位表行数 |
| 17 / 18 | 字典树节点数 / 边数 |
| 19 | Unicode 字符类别区间数 |
| 20 / 21 | 规范分解记录数 / 分解码点池长度 |
| 22 / 23 | 非零组合类别记录数 / 规范组合记录数 |
| 24–39 | 下列 16 个数据区的偏移 |

## 数据区

| 顺序 | 数据区 | 表示 |
| --- | --- | --- |
| 0 | token 偏移 | `总数 + 1` 个 `uint32`，指向字节池；严格递增，没有空 token |
| 1 | 字节池 | token 的字节连续存放，无终止符 |
| 2 | token 类型 | `总数` 个字节：0 普通、1 字节回退 `<0xXX>`、2 特殊；字节回退 token 的字节值由内容 `<0xXX>` 推出，不另存 |
| 3 | 词表哈希 | `uint32`，0 为空，否则为 `token_id + 1` |
| 4 | 合并偏移 | 仅 `rank`：`总数 + 1` 个 `uint32`，指向本 token 的合并规则 |
| 5 | 合并规则 | 仅 `rank`：每条一个 `uint32`，高 24 位是排名，低 8 位是左段的字节长度 |
| 6 | 分数 | 仅 `score`：`总数` 个 `float32`，必须是有限值 |
| 7 | 初始单位 | 每行 2 个 `uint32`：键、ID，按键严格升序 |
| 8 | 字典树节点 | 每项 3 个 `uint32`：首条边、边数、终点 ID + 1（0 表示不是终点） |
| 9 | 字典树边 | 每项 2 个 `uint32`：字节标签、子节点；同一节点内按标签排序，子节点编号大于父节点 |
| 10 | 结束 token | 升序、无重复的 token ID，至少一项 |
| 11 | Unicode 字符类别 | 每行 3 个 `uint32`：起止码点（含）、类别位（字母 1、数字 2、空白 4），按码点升序且互不重叠 |
| 12 | 规范分解索引 | 每行 3 个 `uint32`：码点、池偏移、码点数（1 到 4） |
| 13 | 分解码点池 | 已递归展开的规范分解 |
| 14 | 组合类别 | 每行 2 个 `uint32`：码点、非零 canonical combining class |
| 15 | 规范组合 | 每行 3 个 `uint32`：首、次、合成码点，按前两项排序 |

不适用的数据区长度为 0；这仍然是一个区段，偏移照常记录。Unicode 区段只在需要时写入：
字符类别表要求 `pre_tokenizer` 为 `byte_level_qwen2`，规范分解、分解池、组合类别和规范组合四张表要求 `normalizer` 为 `nfc`，
不需要时必须为空，需要时必须非空。Hangul 的分解与合成使用公式，不逐音节存表。

不另存“解码后的字节”：`metaspace_byte_fallback` 在解码时把 `▁` 换成空格，所以字节池只存一份。

### token、类型与哈希

- 字节池里存的是 token 的真实字节。`byte_level` 的词表在写入时已从 GPT-2 字节映射还原成原始字节；
  只出现在 `added_tokens` 的 token 存它的原始 UTF-8 内容（字节级时必须全是可打印 ASCII，使两种写法一致）。
- 特殊 token（类型 2）取自 `added_tokens` 中 `special` 为真的项，解码时跳过。非特殊的 added token 与普通 token 一样解码。
- 字节回退 token（类型 1）是 `<0xXX>`（大写十六进制）形式的普通 token；词表里出现小写十六进制形式的同类 token 时，工具报错。
  开启 `byte_fallback` 时 256 个都必须存在于 BPE 词表（`model.vocab`）中；只出现在 `added_tokens` 的不算，
  因为 HF 的字节回退只查词表。
- 哈希为 64 位 FNV-1a，取低位定位，线性探测并比较完整字节。登记的是 BPE 词表中的 token；
  只出现在 `added_tokens` 的 token 不登记，所以不会被合并产生。加载时对每个登记的 token 做一次查找，确认哈希与词表一致。

### 合并：一个循环，一个查找路径

每个片段按 `merge_priority` 反复合并**相邻两个单位**：把两个单位的字节拼接（它们在片段中本来就是连续的一段），
在词表哈希中查这个字节串。查到就是候选合并，优先级由 `merge_priority` 决定，优先级高的先合并，优先级相同时取最左。

- `rank`：候选的 token 自己带着合并规则（左段字节长度 + 排名）；只有左段长度与当前左单位相同的规则才算数，排名小的优先。
  所以 token 只接受它登记的切分，与 HF 的 merges 完全一致。工具检查排名小于 2^24，且参与合并的 token 不超过 255 字节，
  超出就报错。`rank` 对任何 BPE 都成立，是默认选择。
- `score`：候选的 token 只带一个分数，优先级由组合键决定，依次比较：分数（高者先）、左段的 Unicode 码点数
  （多者先）、右段的码点数（多者先）、合成 token 的 ID（小者先）；键完全相同时取最左。码点数是码点个数，
  不是字节数，在合并过程中随符号累加。分数来自 SentencePiece 的 `tokenizer.model`，无法由 merges 的排名推算。
  工具只在调用时提供了 `--sentencepiece-model`，**并且**同时满足下面两项时写入 `score`，任一不满足就报错，
  不会悄悄改用 `rank`：
  1. merges 的集合与“两段都在词表中、拼起来也在词表中”的全部切分完全相同；
  2. 把 merges 按上面的组合键排序后，与 HF 的 merges 顺序逐条相同，并且没有并列。

  这样候选合并的先后顺序与 HF 的排名完全相同，等价性在生成时被证明，不依赖测试语料。
  只看分数不够：Gemma 的 514,906 条规则里只看分数会有 278,657 处并列；用字节数代替码点数会有 465 处与 HF 顺序冲突；
  而加上码点数和 ID 后零冲突、零并列。`verify` 对照 HF 只作为补充确认。

初始单位表：字节级是 256 个单字节 token（键 0 到 255）；字符级是“单字符 token → ID”的有序表。
字符级时字符不在表中：`byte_fallback` 开启则按字节写成 `<0xXX>` token（这些单位不参与合并），关闭则编码失败。

### 编码流程与规模

1. 输入必须是有效 UTF-8，长度不超过 `kMaxEncodeBytes`（1 MiB）。
2. 在**原始文本**上，按最左、最长规则用字典树切出 `added_tokens`（特殊与非特殊的都识别），其余片段各自走完整流程，不跨片段合并。
3. 规范化：`replace_space_metaspace` 把每个空格换成 3 字节的 `▁`；`nfc` 做 Unicode 9 的 NFC。
4. 预分词：`byte_level_qwen2` 按 Qwen2 的正则切分（手写的分支顺序、贪婪与回退规则，不是通用正则解释器）；每个词的字节是初始单位。
5. 按上面的规则合并，输出 ID。不自动添加 BOS。

规范化后的长度按输入的 3 倍计算；所有偏移和长度使用 `uint32_t`/`size_t` 并做溢出检查，
`kMaxEncodeBytes` 的 3 倍仍在 `uint32_t` 范围内。工作区按输入长度动态分配。

### 解码

- `metaspace_byte_fallback`：先把非特殊 token 中的 `▁` 换成空格；连续的字节回退 token 凑成一段字节，
  整段是有效 UTF-8 就原样保留，否则**段内每个字节**各替换为一个 U+FFFD（HF `ByteFallback` 的规则）。
  被跳过的特殊 token 不打断字节段。字节编码的 `▁` 保持原样，因为替换先于字节处理。
- `byte_level`：拼接所有非特殊 token 的字节，对整体做有损 UTF-8 解码，每个最大非法子序列替换为一个 U+FFFD。
  跨 token 的字节可以组成同一个字符。NFC 会改变部分输入的字节，编码后再解码不保证还原原文。
- 接口：`AppendTokenBytes` 追加单个 token 的原始字节（特殊 token 追加空串），供流式拼接。
  `Decode` 对完整序列解码。仅当序列由有效 UTF-8 构成时，逐个 `AppendTokenBytes` 的拼接结果才等于 `Decode`；
  含非法字节时 `Decode` 输出 U+FFFD，`AppendTokenBytes` 输出原始字节，两者按设计不相等。
  ID 为负数或不小于词表大小时两个接口都返回错误。

### 结束 token 与 Unicode 数据

结束 token 来自模型的 `generation_config.json` 的 `eos_token_id`，是模型事实，不由部署者配置。
`BpeTokenizer::IsEndToken` / `EndTokens` 读取该数据区。

HF 的规范化器使用 Unicode 9，参考实现固定为 `tokenizers==0.23.3`。
工具用固定 SHA-256 的 Unicode 9.0.0 `UnicodeData.txt` 生成 NFC 表（并对照 HF 的 NFD/NFC 校验），
字符类别从该版本 HF 后端的正则实现取得，并要求全部 Unicode 表匹配固定摘要，后端不同则拒绝输出。
Unicode 数据许可见 [Unicode 许可](https://www.unicode.org/license.txt)；生成的文件不入库。

## 生成、验证与更新

```bash
python3 tools/prepare_bpe_tokenizer.py compile --tokenizer-json T --generation-config G \
    [--sentencepiece-model S] [--unicode-data U] --output model.eftok
python3 tools/prepare_bpe_tokenizer.py fixture {metaspace,byte_level} --output F   # 合成小词表，只用标准库
python3 tools/prepare_bpe_tokenizer.py golden --tokenizer-json T --output model.golden.json
python3 tools/prepare_bpe_tokenizer.py verify --tokenizer-json T --tokenizer-file model.eftok \
    --driver build/edgeflow_bpe_tokenizer_driver
```

`golden`、`verify` 和写 Unicode 区段需要 `tokenizers==0.23.3`。`verify` 用 C++ 驱动
（`tests/support/bpe_tokenizer_driver.cpp`）对随机 Unicode、最长 64 KiB 的文本、ChatML 与 Gemma 模板文本、随机 ID 序列
逐项对照 HF 的编码和解码，必须零差异。用另一个模型的文件去比对，必须报大量差异。

更换模型版本时：更新 `configs/asset_manifest.json` 中的源文件版本和 SHA-256，重新生成并运行 `verify`，
刷新 `.eftok` 的 SHA-256 和 `tests/fixtures/bpe_tokenizer/` 中的样例。两个 `fixture` 子命令在构建时生成默认门禁用的合成文件。
