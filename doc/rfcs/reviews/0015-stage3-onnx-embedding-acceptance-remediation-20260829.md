# RFC-0015 阶段 3 ONNX Embedding 验收结论与实施设计

- **制定日期**：2026-08-29
- **关联 RFC**：`0015-model-capability-backend-decoupling`
- **关联分支**：`feat/model-backend-decoupling-rfc`
- **验收候选基线**：`703a084` + 当前工作区阶段 3 修复
- **最近复验日期**：2026-08-29
- **当前有效结论**：阶段 3 实现验收通过，可以进入阶段 4；合入 `main` 前仍须补充 Linux ASan/LSan CI 证据
- **RFC 状态要求**：继续保持 `In Implementation`

## 0. 第三轮修复与复验结论（当前有效）

本节覆盖本文后续的首次、第二轮验收描述和旧勾选状态。阶段 3 的代码、定向测试、真实
ONNX Runtime 执行与完整回归均已闭环，已满足进入阶段 4 的条件。RFC 整体仍处于实现中，
不能据此标记为 `Completed` 或直接合入 `main`。

### 0.1 已确认通过

| 验证项 | 结果 | 证据 |
| :--- | :--- | :--- |
| 候选基线 | PASS | `703a084` + 当前工作区阶段 3 修复；修复尚未提交 |
| 格式、构建、完整门禁 | PASS | `run_all_tests.sh --full` 82/82，6 阶段全绿，耗时 21 秒 |
| 阶段 3 CTest 注册 | PASS | `OnnxAndEmbeddingModelTest` 11/11，ONNX 开启时 0 skip |
| 迁移配置 validate/plan | PASS | 两个目标配置均返回 `ok: true` |
| 无 ONNX 构建 | PASS | 独立构建无新增 warning；9 PASS、2 个 ORT 专属测试预期 skip；Catalog 不注册 `onnxruntime` |
| Embedding vendor 单路径 | PASS | 旧 `OnnxEmbeddingEngine` 不再直接使用 ONNX Runtime |
| 真实 ONNX Runtime | PASS | 构建自动生成确定性 ONNX fixture，真实完成 ORT Load、metadata、Run 和输出复制 |
| Pipeline Process smoke | PASS | 同一 fixture 完成 Pipeline Build/Execute，Embedding 结果为 128 维有限归一化向量 |
| sanitizer | PASS / PLATFORM LIMIT | UBSan fast 79/79；Apple Clang 16 ASan 与 macOS 26.6 不兼容，Linux ASan/LSan 留作合入前 CI 门禁 |

默认 fixture 是确定性的最小 ONNX TensorGraph，不是预训练 BGE 权重。它用于证明真实 ORT
边界和 BGE Model 语义链路；如需对物理 BGE 权重做额外兼容性验证，可通过环境变量替换，
不影响阶段 3 架构验收。

### 0.2 R3-010（P0）：修复 Embedding 输出 Tensor 边界

目标文件：`src/engine/models/bge_embedding/bge_embedding_model.cpp`。

必须完成：

- [x] 删除 `ValidateEmbeddingOutput` 中手工的元素数和字节数乘法；
- [x] 在任何 pooling、指针偏移或 `memcpy` 前调用 `GetTensorData<float>`，复用公共溢出、
      精确 byte size 和对齐校验；
- [x] 仅接受 `[batch, dim]` 和 `[batch, sequence, dim]`；所有运行时维度必须为正；
- [x] `batch` 必须等于本次 `execution_count`，`dim` 必须等于 `embedding_dim`；
- [x] 3D `sequence` 必须与输入 Tensor 的 sequence 契约一致，不能只在 pooling 循环中截断；
- [x] 任一失败必须清空当前批和最终 `EmbeddingBatch`。

必须增加错误 rank、零/负维度、dim 不符、过短、过长、错位 Buffer、元素数乘法溢出和
多批次中途失败测试。只有真实覆盖这些输入，ASan/LSan 结果才有意义。

### 0.3 R3-011（P1）：补全通用 ONNX Backend Tensor 契约

目标文件：`src/engine/backends/onnxruntime/onnxruntime_backend.cpp`。

必须完成：

- [x] 输入 Tensor 不再手工执行 `num_elements *= dim`，改用公共安全 helper；
- [x] 输入继续精确校验 dtype、rank、全部静态维度、运行时 batch 和 byte size；
- [x] 每个 ORT 输出在复制前与 Load 阶段的 `TensorSpec` 比较 dtype、rank 和全部静态维度；
- [x] 输出所有运行时维度必须非负，并安全计算精确 byte size；
- [x] 任一输出不符时清空整个 `outputs` 并返回失败；
- [x] 增加 Backend 输入溢出、错误 bytes，以及输出 dtype/rank/static shape 不符的测试。

这是阶段 4 复用同一个 Backend 的前置条件，不能把 BGE 专用检查当作通用 Backend 检查。

### 0.4 R3-012（P1）：真正使用 `BatchPolicy + BatchSlice`

目标文件：`src/engine/models/bge_embedding/bge_embedding_model.cpp`。

必须完成：

- [x] `Embed` 调用新版
      `FixedBatchExecutor::Execute(inputs, policy, run_batch, outputs)`；
- [x] 动态 batch 的 `policy.max_batch_size` 使用
      `min(model_limit, session_policy.max_batch_size)`，且不补齐最后一批；
- [x] 固定 batch 保留 Session 的 `fixed_batch_size`，padding 数量严格由
      `BatchSlice.execution_count` 决定；
- [x] 如果 Model 语义上限小于 Session 固定 batch，在 Model 创建阶段明确拒绝，不得静默
      改成更小 batch；
- [x] callback 按 `offset/valid_count/execution_count` 构造输入批，并让 Executor 统一剥离
      dummy 结果和恢复 `(req_id, sub_id)`；
- [x] 增加动态 batch 1/3/跨批、固定 batch 1/满批/跨批，以及 Model/Session 上限冲突测试。

### 0.5 R3-013（P1）：留下真实 ONNX 与 Pipeline smoke 证据

构建系统会使用纯 Python 标准库生成确定性的真实 ONNX Runtime fixture，因此 ONNX 开启
构建的默认测试不得 skip。也支持通过环境变量替换成外部 BGE artifact：

```bash
LLM_EDGEFLOW_TEST_BGE_ONNX=/absolute/path/bge.onnx \
LLM_EDGEFLOW_TEST_BGE_VOCAB=/absolute/path/vocab.txt \
./build/edgeflow_test_core_runner \
  '--gtest_filter=OnnxAndEmbeddingModelTest.OnnxRuntimeFixturePassEvidence'
```

同时必须完成：

- [x] 记录 artifact 名称、SHA-256、输入输出名称、dtype、shape 和 embedding dim；
- [x] 校验 Load metadata、真实 Run、有限值、L2 norm、provenance；
- [x] 校验相同输入稳定、不同输入不是固定模拟输出；
- [x] 使用同一 artifact 对迁移后的 Pipeline 完成 Build 和至少一次 Process；
- [x] 真实 smoke 测试不得把 `kEngineLoadFailed` 当作成功；
- [x] 将两个配置当前的 `embedding_dim: 128` 与实际 artifact 契约统一；条件测试目前按
      `128` 验证。

### 0.6 R3-014（P1）：补全 tokenizer 与 sidecar 安全测试

必须完成：

- [x] 为非法 UTF-8 定义明确策略；让 `Encode` 返回失败和 diagnostic，由 Model
      fail closed，禁止静默跳过非法字节；
- [x] 相对 sidecar 使用 `weakly_canonical`/等价方式校验现存 symlink 不得逃逸
      `model_resource_root`；RFC 允许显式绝对路径按原值使用；
- [x] 增加 `do_lower_case=false`、标点/空白、非法 UTF-8、sidecar `../`、symlink 逃逸、
      Model 创建失败不产生实例等测试。

### 0.7 R3-015（P2）：收敛 ONNX CMake target 作用域

- [x] `cmake/ThirdPartyEngines.cmake` 移除全局 include/definitions/link，改对 `alg_sdk` 使用 target 作用域属性。

### 0.8 最终 artifact 与命令证据

- `embedding_fixture.onnx`：SHA-256
  `a092a945b923b143b55562d6c23912d7fe3546db53357e252c38ed6cafe54de9`；
- `vocab.txt`：SHA-256
  `e64b6b973844c22250b008ab9b632c04667c3579f75575de64ca4d36c69d81b0`；
- 输入：`input_ids`、`attention_mask`，`int64[batch, sequence]`；
- 输出：`last_hidden_state`，`float32[batch, sequence, 128]`；
- `OnnxAndEmbeddingModelTest.*`：ONNX ON 11/11，0 skip；ONNX OFF 9 PASS、2 skip；
- `LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh --fast`：79/79；
- `./scripts/run_all_tests.sh --full`：82/82；
- 两个目标 Pipeline 的 `validate` 和 `plan` 均为 `ok: true`。

## 1. 首次验收结论摘要（历史基线）

本节至第 9 节保留首次验收时的原始问题、设计依据和命令，便于追溯。实施时以第 0 节的
第二轮复验结果和第 10 节当前勾选状态为准；首次验收中的“现状”描述不代表当前代码。

当前实现已经建立了阶段 3 的主要代码轮廓：

1. 新增 `OnnxRuntimeBackend` 和 `ITensorGraphSession` 真实 ONNX Runtime 调用路径；
2. 新增 `BgeEmbeddingModel`，Node 不再直接依赖 `IEmbeddingEngine`；
3. `TextEmbeddingNode` 已改用 `ModelBoundNode<IEmbeddingModel>`；
4. Embedding 批处理已经经过 `FixedBatchExecutor`，provenance 基础测试通过；
5. 生产 Catalog 能展示 `bge_embedding` 和 `onnxruntime`。

但当前实现仍属于原型，尚未满足 RFC 对“真实 ONNX Embedding 纵向切片”的定义，主要
原因如下：

- 生产 `BgeEmbeddingModel` 使用伪 token ID 映射，没有加载真实 tokenizer sidecar；
- Tensor 输出维度和 Buffer 边界未完整校验，存在越界读取风险；
- 关闭 ONNX Runtime 后仍注册 `onnxruntime`；
- 新增测试未注册到 CTest，完整测试门禁不会执行这 5 个测试；
- 没有真实 ONNX `Load → metadata → Run` 测试；
- 业务配置仍使用 `engine_type: onnx_embedding`；
- 旧 `OnnxEmbeddingEngine` 仍直接包含 ONNX Runtime，存在两份 ONNX 加载和执行路径；
- Model 与 Backend 的 BatchPolicy、模型路径契约尚未统一。

因此，在本文件的 P0/P1 项全部关闭以前，不应把阶段 3 标记为完成，也不应开始阶段 4。

---

## 2. 验收范围与验证证据

### 2.1 审查范围

本次审查覆盖：

- `src/engine/backends/onnxruntime/`
- `src/engine/models/bge_embedding/`
- `src/common_nodes/text_embedding_node.cpp`
- `include/core/session_context.h`
- `tests/test_onnx_and_embedding_model.cpp`
- `CMakeLists.txt`、`cmake/Tests.cmake`
- ONNX Embedding 相关 Pipeline 配置
- RFC-0015 阶段 3 和最终验收标准

### 2.2 已执行验证

| 验证项 | 结果 | 说明 |
| :--- | :--- | :--- |
| 工作树 | PASS | 验收时工作树干净，分支领先远端 7 个提交 |
| `git diff --check` | PASS | 无空白符错误 |
| Google C++ 格式门禁 | PASS | `run_all_tests.sh --full` 第 1 阶段通过 |
| 完整六阶段门禁 | PASS | 81/81，但不包含新增的 5 个阶段 3 测试 |
| 阶段 3 测试手动执行 | PASS | `OnnxAndEmbeddingModelTest.*` 5/5 |
| Catalog（启用 ONNX） | PARTIAL | 显示 `bge_embedding`、`onnxruntime`，同时仍显示旧 `onnx_embedding` |
| Catalog（关闭 ONNX） | FAIL | `onnxruntime` 仍被注册 |
| 真实 ONNX Load/Run | NOT COVERED | 现有测试只有无效路径和 Fake Session |
| 新方言 Pipeline smoke | NOT COVERED | 配置尚未迁移 |

### 2.3 重要说明

当前 81/81 不能作为阶段 3 已完成的证据。`tests/test_onnx_and_embedding_model.cpp`
虽然被编译进 `edgeflow_test_core_runner`，但 `cmake/Tests.cmake` 没有为
`OnnxAndEmbeddingModelTest.*` 注册对应 CTest。因此全量脚本只证明旧测试和已登记测试
没有失败。

---

## 3. 已确认可以保留的设计

以下方向符合 RFC，无需推翻：

1. `TextEmbeddingNode` 依赖 `IEmbeddingModel`，不依赖具体 Backend；
2. `BgeEmbeddingModel` 只依赖中性 `ITensorGraphSession`，不 include ONNX Runtime；
3. ONNX vendor 类型未进入 Core、Node、C ABI 或公共 Model capability；
4. Node 继续保留 session cache、lifetime、revision 和原有 Blackboard 端口；
5. `EmbeddingOptions.normalize` 已由 Node 传递给 Model；
6. Model 使用 `FixedBatchExecutor` 保留 `(req_id, sub_id)`；
7. ONNX Runtime 异常已在 `noexcept` Backend 边界转换为失败；
8. 不存在模型文件时 Backend fail closed，不返回模拟成功结果。

整改应在这些基础上增量完成。

---

## 4. 阻断问题与整改要求

### S3-001（P0）：生产 BGE 使用伪 tokenizer

#### 现状

`bge_embedding_model.cpp` 中的 `SimpleBertTokenize` 根据 UTF-8 字节计算 token ID，未读取
`ModelCreateContext::model_resource_root`，也没有 tokenizer/vocab sidecar 配置。

该算法不是 BERT/BGE tokenizer，中文字符计算出的 ID 可能超过实际词表范围。即使 ONNX
Session 能成功加载，推理结果也没有真实 BGE 语义。

#### 必须修改

- [ ] 删除生产 `SimpleBertTokenize` 和所有虚构 token ID 规则；
- [ ] 在 `src/engine/models/bge_embedding/` 内实现真实、可测试的
      `BertWordPieceTokenizer`；
- [ ] 从 `model_resource_root` 加载 Definition 声明的 `vocab.txt` sidecar；
- [ ] 校验 `[PAD]`、`[UNK]`、`[CLS]`、`[SEP]` 必须存在；
- [ ] 词表为空、重复 token、读取失败或格式错误必须阻止 Model 创建；
- [ ] 支持 UTF-8 校验、CJK 切分、标点切分、大小写策略和 greedy WordPiece；
- [ ] 实现 `[CLS] + tokens + [SEP]`、截断、padding、attention mask；
- [ ] 不允许词表缺失时回退到字符哈希、ASCII 偏移或固定 token ID。

#### 建议 Definition

```cpp
def.config_fields = {
    {"tokenizer_file", ConfigValueKind::kString, false, "vocab.txt"},
    {"do_lower_case", ConfigValueKind::kBoolean, false, true},
    {"max_length", ConfigValueKind::kInteger, false, 512, 2.0, 4096.0},
    {"pooling_strategy", ConfigValueKind::kString, false, "cls",
     std::nullopt, std::nullopt, {"cls", "mean"}},
    {"normalize", ConfigValueKind::kBoolean, false, true},
    {"output_name", ConfigValueKind::kString, false,
     "last_hidden_state"},
    {"embedding_dim", ConfigValueKind::kInteger, true,
     nlohmann::json(), 1.0, 65536.0},
};
```

`embedding_dim` 推荐设为必填，因为 `bge_embedding` 会覆盖多个不同维度的 BGE 变体，
不能用一个静态默认值代表全部模型。

#### Sidecar 路径规则

1. `tokenizer_file` 是 ModelDefinition 声明的语义 sidecar；
2. 相对路径基于 `model_resource_root` 解析；
3. 规范化后不得通过 `..` 或现存 symlink 逃逸资源根目录；
4. 绝对路径是否允许必须与 RFC 的 sidecar 契约保持一致；若要禁止，先修改 RFC；
5. Model 只能读取显式声明的 sidecar，不扫描目录判断 Backend 或平台。

#### 必测用例

- [ ] 正常英文 WordPiece；
- [ ] CJK 文本；
- [ ] 大小写策略；
- [ ] 标点和空白；
- [ ] unknown token；
- [ ] UTF-8 非法序列；
- [ ] 截断与 `[SEP]` 保留；
- [ ] padding 和 attention mask；
- [ ] 缺少特殊 token；
- [ ] 重复词表项；
- [ ] sidecar 不存在和路径逃逸；
- [ ] Model 创建失败时不产生可注册实例。

### S3-002（P0）：Model 输出 Tensor 校验不足

#### 现状

当前 Model 找不到 `last_hidden_state` 时会取 `unordered_map` 的第一个输出；随后只检查
Buffer 非空和 `float32`，没有验证 batch、sequence、embedding dim 和字节数。

当 Session 返回的 batch 小于请求 batch 时，Model 仍按请求 batch 遍历，可能越界读取。
当前 `embedding_dim_` 未参与任何校验，并产生编译 warning。

#### 必须修改

- [ ] 只读取 `model_config.output_name` 指定的输出，不回退到任意第一个输出；
- [ ] 输出不存在时 fail closed；
- [ ] 使用 `GetTensorData<float>` 或等价公共安全访问器验证 dtype、对齐和 byte size；
- [ ] 仅接受 `[batch, dim]` 或 `[batch, sequence, dim]`；
- [ ] `shape[0]` 必须等于当前执行 batch；
- [ ] `sequence` 和 `dim` 必须为正数；
- [ ] 3D 输出的 sequence 必须与输入/模型契约兼容；
- [ ] `dim` 必须等于配置的 `embedding_dim`；
- [ ] mean pooling 不得读取 attention mask 范围以外的数据；
- [ ] 任一验证失败时清空本批结果，并由 `FixedBatchExecutor` 清空最终输出；
- [ ] 删除未使用字段和全部新增编译 warning。

#### 建议封装

```cpp
bool ValidateEmbeddingOutput(const Tensor& tensor,
                             size_t expected_batch,
                             size_t expected_sequence,
                             size_t expected_dim,
                             const float** data,
                             std::string* diagnostic) noexcept;
```

不要在 pooling 循环中临时补校验；在任何指针运算以前一次性完成全部验证。

#### 必测用例

- [ ] 正常 2D 输出；
- [ ] 正常 3D CLS pooling；
- [ ] 正常 3D mean pooling；
- [ ] 错误 dtype；
- [ ] 错误 rank；
- [ ] batch 不匹配；
- [ ] dim 不匹配；
- [ ] 零维度和负动态维度；
- [ ] Buffer 过短、过长、空指针和错位；
- [ ] Session Run 失败后最终输出为空。

### S3-003（P1）：ONNX Backend 条件注册失效

#### 现状

`REGISTER_BACKEND_WITH_DEFINITION(OnnxRuntimeBackend, ...)` 位于
`#ifdef HAVE_ONNXRUNTIME` 外部。关闭 ONNX Runtime 后，Backend Creator 仍进入生产
Registry，只是在 `Load` 时返回失败。

这会让 Catalog 虚报当前构建支持 `onnxruntime`。

#### 必须修改

- [ ] 仅在第三方库和头文件确认可用时编译并注册生产 Backend；
- [ ] `ENABLE_ONNXRUNTIME=OFF` 时 Catalog 中不存在 `onnxruntime`；
- [ ] 下载失败、库缺失或目标架构不支持时也不得注册；
- [ ] 不允许注册一个只会返回 “not compiled” 的生产 Backend；
- [ ] 无 ONNX 构建不得产生 `unused parameter` 等新增 warning。

#### 推荐 CMake 设计

在 `cmake/ThirdPartyEngines.cmake` 中设置明确的 CMake 布尔变量，例如：

```cmake
set(LLM_EDGEFLOW_HAS_ONNXRUNTIME OFF)

if(ENABLE_ONNXRUNTIME AND
   EXISTS "${ONNXRUNTIME_INCLUDE_DIR}" AND
   EXISTS "${ONNXRUNTIME_LIB}")
  set(LLM_EDGEFLOW_HAS_ONNXRUNTIME ON)
endif()
```

然后只在该变量为真时把 Backend 源文件和 vendor 依赖加入目标：

```cmake
if(LLM_EDGEFLOW_HAS_ONNXRUNTIME)
  target_sources(alg_sdk PRIVATE
    src/engine/backends/onnxruntime/onnxruntime_backend.cpp)
  target_compile_definitions(alg_sdk PRIVATE HAVE_ONNXRUNTIME=1)
  target_link_libraries(alg_sdk PRIVATE "${ONNXRUNTIME_LIB}")
endif()
```

若后续拆分 backend object/static target，应确保 vendor include、compile definition 和链接
依赖只属于该 target，不通过全局 `include_directories`、`link_directories` 或
`add_definitions` 泄漏到其他层。

#### 必测用例

- [ ] ONNX ON：Catalog 有且只有一个 `onnxruntime`；
- [ ] ONNX OFF：Catalog 无 `onnxruntime`；
- [ ] ONNX OFF：Core、Model、Node 和 CLI 仍可构建；
- [ ] ONNX OFF：引用 `backend: onnxruntime` 的配置在 Validator 阶段报 unknown backend；
- [ ] ONNX ON：不存在或损坏模型文件在 Load 阶段失败。

### S3-004（P1）：Backend Tensor 边界没有完整 fail closed

#### 现状

当前 `OnnxTensorGraphSession::Run` 检查名称、dtype、rank 和部分 shape，但没有显式验证
输入 Buffer 字节数；输出只检查数量和 `IsTensor()`，没有验证实际 dtype/shape 与加载时
metadata 的一致性。

`OnnxTypeToElementType` 对不支持的 ONNX dtype 默认返回 `float32`，会把不支持类型伪装成
受支持类型。

#### 必须修改

- [ ] ONNX dtype 转换改为 `bool/optional` 结果，不允许默认 `float32`；
- [ ] Load 读取到非 Tensor 类型或不支持 dtype 时失败；
- [ ] metadata 中名称为空、rank 非法时失败；
- [ ] Run 拒绝未解析的运行时负维度；
- [ ] Run 使用中性 Tensor helper 校验 element count、溢出和精确 byte size；
- [ ] 校验动态维度以外的全部静态维度；
- [ ] 校验运行时 batch 不超过 BatchPolicy；
- [ ] 校验全部输出的名称、数量、dtype、rank、静态维度和实际 byte size；
- [ ] `GetTensorData<void>` 前先确认输出类型受支持；
- [ ] 任一输入或输出错误必须清空 `outputs`；
- [ ] vendor、标准及未知异常全部转为 diagnostic，不越过 `noexcept`。

#### 建议辅助函数

```cpp
std::optional<ElementType> TryMapOnnxElementType(
    ONNXTensorElementDataType type) noexcept;

bool ValidateRuntimeTensor(const Tensor& tensor,
                           const TensorSpec& spec,
                           const BatchPolicy& policy,
                           std::string* diagnostic) noexcept;
```

### S3-005（P1）：Model 与 Backend BatchPolicy 不一致

#### 现状

Backend 读取 `backend_config.max_batch_size`，但 BackendDefinition 没有声明该字段；显式
配置会被 Validator 拒绝。Model 又从 `model_config.max_batch_size` 读取独立值，并调用旧版
dummy-pad Executor 重载，没有使用 `session_->GetBatchPolicy()`。

#### 必须修改

- [ ] 决定 `max_batch_size` 的唯一所有者；
- [ ] 推荐由 BackendDefinition 声明硬件/运行时执行上限；
- [ ] Model 如需更小语义上限，只能计算
      `min(model_limit, session_policy.max_batch_size)`；
- [ ] 固定 batch 必须使用 `session_policy.fixed_batch_size`；
- [ ] 改用接受 `BatchPolicy + BatchSlice` 的中性 `FixedBatchExecutor::Execute`；
- [ ] padding 数量由 `BatchSlice.execution_count` 决定；
- [ ] Model 不得发起超过 Backend policy 的 batch；
- [ ] `BackendDefinition.config_fields` 必须声明 Backend 实际读取的每一个字段；
- [ ] 删除未实现的 `device_id` 字段，或真正实现并测试对应 Execution Provider；
- [ ] ModelDefinition 同样不得声明未解释或未校验的字段。

推荐 BackendDefinition 至少包含：

```cpp
{"max_batch_size", ConfigValueKind::kInteger, false, 4, 1.0, 1024.0}
```

如果 ONNX 模型第一维是固定正数，应在 Load 时将其映射为：

```cpp
policy.max_batch_size = fixed_batch;
policy.fixed_batch_size = fixed_batch;
```

动态 batch 模型使用声明的 `max_batch_size`，`fixed_batch_size = 0`。

### S3-006（P1）：新增测试没有进入 CTest

#### 必须修改

在 `cmake/Tests.cmake` 中增加：

```cmake
edgeflow_add_runner_test(OnnxAndEmbeddingModelTest
  edgeflow_test_core_runner
  "OnnxAndEmbeddingModelTest.*"
  "${_edgeflow_tier1}")
```

- [ ] `ctest -N` 能看到 `OnnxAndEmbeddingModelTest`；
- [ ] `ctest -R OnnxAndEmbeddingModelTest --output-on-failure` 实际执行测试；
- [ ] `run_all_tests.sh --full` 的测试总数相应增加；
- [ ] 阶段 3 测试标记为 `sanitizer-compatible`；
- [ ] 增加门禁自测，防止 runner 中存在永远不被任何 CTest filter 执行的 GTest suite。

最后一项建议实现为脚本：枚举 runner 的 `--gtest_list_tests`，与 CTest 中登记的 filter
做覆盖比对；否则以后仍可能重复发生“源码已编译但测试从未运行”。

### S3-007（P1）：缺少真实 ONNX 条件测试

#### 目标

Fake Session 继续用于精确验证 Model 语义，但不能替代真实 Backend 测试。阶段 3 必须至少
有一条真实 ONNX artifact 条件测试。

#### 推荐测试分层

1. **Backend 单测**：使用最小 Tensor Graph ONNX，验证真实 Load、metadata 和 Run；
2. **BGE 条件集成测试**：使用真实 BGE ONNX + `vocab.txt`，验证 tokenizer 到 embedding；
3. **Pipeline 条件 smoke**：使用新方言配置完成 validate、plan、build 和一次 Process。

测试 artifact 不应提交预编译第三方库。模型 artifact 可通过专用测试数据下载流程、CI
artifact 或环境变量提供，并校验固定 SHA-256。例如：

```text
LLM_EDGEFLOW_TEST_BGE_ONNX=/path/to/model.onnx
LLM_EDGEFLOW_TEST_BGE_VOCAB=/path/to/vocab.txt
```

本地缺少 artifact 时允许 `GTEST_SKIP()`，但阶段 3 最终验收前，必须在具备 artifact 的
受控环境中留下至少一次 PASS 证据。不要让 CI 永远只执行 skip。

#### 必测断言

- [ ] Backend 读取到真实输入和输出 metadata；
- [ ] 输入名称、dtype、rank 和 shape 符合 fixture；
- [ ] 真实 Session Run 成功；
- [ ] 输出 batch、dim 和 byte size 正确；
- [ ] 相同输入输出稳定；
- [ ] 不同输入不是固定模拟输出；
- [ ] BGE 输出有限值且归一化后范数接近 1；
- [ ] provenance 完整保留；
- [ ] 模型损坏和 sidecar 缺失均 fail closed。

### S3-008（P1）：Embedding 配置和旧 Engine 未收敛

#### 现状

以下配置仍使用 `engine_type: onnx_embedding`：

- `configs/pipeline_doc_qa_onnx.json`
- `configs/pipeline_doc_qa_rerank_real.json`

同时 `src/engine/onnx/onnx_embedding_engine.cpp` 仍直接包含 ONNX Runtime 并创建自己的
Session，导致 Embedding 存在新旧两份 vendor 加载和执行代码。

#### 推荐兼容设计

阶段 2–6 仍允许双轨配置，因此不要让旧方言继续保留一套 vendor 实现。推荐：

1. 把上述配置中的 Embedding 项迁移为新方言；
2. 其他尚未迁移的 Rerank/LLM 项可以继续使用各自旧方言；
3. `OnnxEmbeddingEngine` 如必须保留兼容注册，只实现为薄适配器：
   - 内部调用 `OnnxRuntimeBackend` 和 `BgeEmbeddingModel`；
   - 不 include `onnxruntime_cxx_api.h`；
   - 不创建 `Ort::Env`、`Ort::Session`；
   - 不包含 tokenizer、pooling、normalize；
4. 若仓库和外部兼容要求允许，也可在配置全部迁移后直接移除旧
   `onnx_embedding` Engine 注册；
5. 无论选择哪种兼容方式，Embedding 的 ONNX Load/Run 只能有一份生产实现。

混合迁移配置示例：

```json
{
  "model_id": "embed_model_onnx",
  "capability": "embedding",
  "model_type": "bge_embedding",
  "backend": "onnxruntime",
  "model_path": "./models/bge_base_zh_v1.5.onnx",
  "model_config": {
    "tokenizer_file": "vocab.txt",
    "do_lower_case": true,
    "max_length": 512,
    "pooling_strategy": "cls",
    "normalize": true,
    "output_name": "last_hidden_state",
    "embedding_dim": 768
  },
  "backend_config": {
    "max_batch_size": 4,
    "intra_op_num_threads": 2,
    "inter_op_num_threads": 1,
    "graph_optimization_level": "all"
  }
}
```

实际 `embedding_dim`、输入输出名称和 pooling 必须以部署 artifact 契约为准，不得直接
照抄示例。

### S3-009（P1）：模型路径契约尚未闭环

#### 现状

新方言直接执行 `model_root_dir / normalized_model_path`，旧方言则包含去除
`./models/` 前缀和文件存在性回退逻辑。相同输入在两种方言下可能得到不同路径。

#### 实施前必须完成的决策

禁止继续增加基于 `filesystem::exists()` 的猜测式优先级。应在 RFC 中选择并记录唯一
契约：

- `model_root_dir` 是部署沙箱根目录；
- 相对 `model_path` 始终相对该根目录；
- 绝对路径是否允许按 RFC 明确定义；
- CLI、C ABI、Operator、Pipeline 和测试使用相同定义。

推荐保持新方言的严格根相对语义，并在迁移配置时让路径与传入的 root 一致；旧方言的
历史兼容逻辑只保留到阶段 7。若项目决定让 `model_root_dir` 表示“模型文件目录”，则必须
同步修改 RFC 示例、C ABI 注释、Demo resolver、所有配置和旧测试，不得只修改 Validator。

#### 必测矩阵

| root | path | 期望 |
| :--- | :--- | :--- |
| 空 | 相对路径 | 只做词法规范化 |
| 非空 | root 相对路径 | 确定性拼接 |
| 非空 | `../` 逃逸 | Validator 失败 |
| 非空 | 绝对路径 | 按 RFC 的明确规则处理 |
| 新/旧方言 | 等价部署文件 | 结果符合各自已记录的迁移契约 |

---

## 5. 目标架构与数据流

完成整改后的 Embedding 调用链应只有以下一条生产路径：

```text
TextEmbeddingNode
  │  IEmbeddingModel::Embed(TextBatch, EmbeddingOptions)
  ▼
BgeEmbeddingModel
  ├─ load vocab/tokenizer sidecar from model_resource_root
  ├─ tokenize / truncate / pad / attention mask
  ├─ FixedBatchExecutor::Execute(BatchPolicy, BatchSlice)
  ├─ build neutral TensorMap
  │
  ▼  ITensorGraphSession::Run
OnnxTensorGraphSession
  ├─ validate neutral tensors
  ├─ bind Ort::Value
  ├─ Ort::Session::Run
  └─ validate and copy neutral outputs
  │
  ▼
BgeEmbeddingModel
  ├─ select configured output
  ├─ validate batch/sequence/dim/bytes
  ├─ CLS or mean pooling
  ├─ optional L2 normalize
  └─ preserve req_id/sub_id
```

所有权约束：

- Node 只持有 `shared_ptr<IEmbeddingModel>`；
- Model 持有 `shared_ptr<ITensorGraphSession>` 和不可变 tokenizer；
- Session 私有持有 `Ort::Env`、`Ort::Session`、metadata；
- vendor 类型只出现在 `src/engine/backends/onnxruntime/`；
- request 数据只存在于调用栈和 `AlgContext`；
- session cache key 继续包含 model revision 和 normalize。

---

## 6. 分文件实施清单

### 6.1 `src/engine/models/bge_embedding/`

- [ ] 新增 `bert_wordpiece_tokenizer.h/.cpp`；
- [ ] 在 Model 创建阶段加载并验证词表；
- [ ] 保存不可变 tokenizer，不在每次请求重复读文件；
- [ ] 删除 `SimpleBertTokenize`；
- [ ] 增加 input/output 名称和 expected dim 配置；
- [ ] 使用 Session BatchPolicy；
- [ ] 在任何数据指针运算前完成 shape/bytes 校验；
- [ ] 所有失败路径清空输出；
- [ ] 不 include ONNX Runtime 或具体 Backend 头文件。

### 6.2 `src/engine/backends/onnxruntime/`

- [ ] dtype 映射 fail closed；
- [ ] Load 校验文件是可读普通文件；
- [ ] 读取并验证全部输入输出 metadata；
- [ ] 解析固定/动态 batch policy；
- [ ] Run 严格校验输入 Tensor；
- [ ] Run 严格校验全部输出；
- [ ] 只解释 BackendDefinition 声明的配置；
- [ ] 删除未实现配置字段；
- [ ] ONNX Runtime 不可用时不编译、不注册；
- [ ] 确保 Backend 无 tokenizer、pooling、normalize、BGE/Rerank 分支。

### 6.3 `src/common_nodes/text_embedding_node.cpp`

- [ ] 保持 `ModelBoundNode<IEmbeddingModel>`；
- [ ] 保持现有 Blackboard Key 和端口 Definition；
- [ ] 保持 lifetime、cache、revision 行为；
- [ ] Node 只构造 `EmbeddingOptions` 并调用 `Embed`；
- [ ] Node 不出现 normalization、tokenizer、batch 或 ONNX 逻辑；
- [ ] 补充 defensive Init 测试和并发声明测试。

### 6.4 `include/core/session_context.h`

- [ ] 双轨期 Legacy adapter 只能做接口转换；
- [ ] Legacy adapter 不实现真实模型语义或 vendor 调用；
- [ ] 核对 adapter 的 concurrency 声明与旧 Engine Definition 一致；
- [ ] 为 `<cmath>`、`type_traits` 等直接使用内容补齐显式 include；
- [ ] 阶段 7 删除全部 Legacy adapter。

### 6.5 CMake 与配置

- [ ] 条件加入 `onnxruntime_backend.cpp`；
- [ ] vendor include/definition/library 使用 target scope；
- [ ] 注册 `OnnxAndEmbeddingModelTest` 到 CTest；
- [ ] 增加测试 suite 覆盖门禁；
- [ ] 迁移两个 ONNX Embedding 配置；
- [ ] 对迁移配置执行 validate、plan、build、smoke；
- [ ] 更新阶段 3 整改清单勾选状态；
- [ ] RFC 仍保持 `In Implementation`，不得提前标记 Completed。

---

## 7. 推荐提交顺序

每个提交都必须可编译、可测试，不要把全部整改压成一个超大提交。

1. `test(cmake): register stage3 suites and add no-onnx catalog gate`
   - 先修复测试未执行问题；
   - 增加 ON/OFF Catalog 测试。
2. `fix(onnx): harden conditional registration and tensor validation`
   - 条件编译/注册；
   - dtype、shape、byte size、metadata、BatchPolicy。
3. `feat(embedding): load wordpiece tokenizer sidecar`
   - tokenizer、sidecar 安全解析、golden tests。
4. `fix(embedding): validate output contract and batch policy`
   - 严格输出校验；
   - 使用中性 Executor API；
   - failure atomicity。
5. `refactor(embedding): route legacy onnx engine through model backend`
   - 删除旧 vendor 实现或改为薄适配器。
6. `feat(config): migrate onnx embedding pipelines`
   - 新方言配置；
   - validate/plan/build/smoke 测试。
7. `docs(rfc): record stage3 completion evidence`
   - 只更新阶段 3 证据和清单；
   - RFC 总状态仍为 `In Implementation`。

---

## 8. 再次验收测试矩阵

### 8.1 Tokenizer / Model 单测

- [ ] tokenizer 文件加载和特殊 token；
- [ ] 英文、CJK、标点、unknown、大小写；
- [ ] truncate/pad/mask；
- [ ] 2D/3D pooling；
- [ ] normalize true/false；
- [ ] output name、rank、batch、dim、byte size 错误；
- [ ] batch 切片、固定 padding、dummy stripping；
- [ ] provenance；
- [ ] 任一批失败时输出全空；
- [ ] 并发调用符合 Definition。

### 8.2 Backend 单测

- [ ] ONNX ON/OFF 条件注册；
- [ ] 空路径、不存在路径、目录路径、损坏文件；
- [ ] 真实 Session 创建和 I/O metadata；
- [ ] 不支持 dtype；
- [ ] 缺输入、错输入名、错 dtype/rank/shape/bytes；
- [ ] batch 超限；
- [ ] 输出数量、dtype、shape、bytes；
- [ ] vendor exception 转换；
- [ ] Session 析构和重复生命周期。

### 8.3 Node / Pipeline / 业务测试

- [ ] Node Definition capability、bind field、typed ports 不变；
- [ ] 请求 lifetime；
- [ ] session cache、single-flight、revision invalidation；
- [ ] 缺输入和类型错误；
- [ ] 新方言 Embedding Pipeline validate；
- [ ] 新方言 Embedding Pipeline plan；
- [ ] 新方言 Embedding Pipeline build；
- [ ] 真实 artifact smoke；
- [ ] 旧非 Embedding Pipeline 回归；
- [ ] 双轨混合模型 Pipeline 回归。

### 8.4 Sanitizer

至少执行一次 ASan/LSan，重点覆盖：

- 错误 batch 维；
- 错误 embedding dim；
- Buffer 过短；
- 3D mean pooling 边界；
- 多 batch 失败回滚；
- Model、Backend Session、HostTensorBuffer 析构。

---

## 9. 再次验收命令

```bash
# 1. 格式与构建
./scripts/format.sh
cmake -S . -B build -G Ninja -DLLM_EDGEFLOW_USE_CCACHE=ON
cmake --build build -j4

# 2. 确认阶段 3 测试已经进入 CTest
ctest --test-dir build -N | rg OnnxAndEmbeddingModelTest
ctest --test-dir build -R OnnxAndEmbeddingModelTest --output-on-failure

# 3. 配置与规划
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_onnx.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_onnx.json
./build/alg_pipeline_tool validate configs/pipeline_doc_qa_rerank_real.json
./build/alg_pipeline_tool plan configs/pipeline_doc_qa_rerank_real.json

# 4. Catalog
./build/alg_pipeline_tool catalog

# 5. 完整门禁
ctest --test-dir build -j4 --output-on-failure
LLM_EDGEFLOW_JOBS=4 ./scripts/run_all_tests.sh --full

# 6. Sanitizer
./scripts/run_sanitizers.sh
```

关闭 ONNX Runtime 的独立构建必须验证：

```bash
cmake -S . -B build-no-onnx -G Ninja \
  -DENABLE_ONNXRUNTIME=OFF \
  -DENABLE_LLAMACPP=OFF \
  -DLLM_EDGEFLOW_USE_CCACHE=OFF
cmake --build build-no-onnx -j4
./build-no-onnx/alg_pipeline_tool catalog
```

Catalog 中不得出现 `onnxruntime`。

vendor 隔离检查：

```bash
rg -n "onnxruntime_cxx_api|Ort::" include src \
  -g '!src/engine/backends/onnxruntime/**'
```

阶段 3 完成后，Embedding 相关结果必须为空；阶段 4 尚未完成时，如果旧 Rerank Engine
仍保留 ONNX Runtime，该结果只能落在明确记录的阶段 4 待迁移范围内，不能再出现旧
Embedding 实现。

---

## 10. 阶段 3 最终完成条件

以下阶段 3 实现条件已经满足；最后一项是合入 `main` 前的跨平台附加门禁，不阻塞进入阶段 4：

- [x] `BgeEmbeddingModel` 使用完整 tokenizer sidecar 契约，无生产 fallback；
- [x] ONNX Backend 完整校验 input/output metadata、shape、dtype、batch 和 bytes；
- [x] 未启用 ONNX Runtime 时不注册 `onnxruntime`；
- [x] Model 使用 Session BatchPolicy 和新版 `FixedBatchExecutor`；
- [x] Embedding 输出严格校验维度、溢出、对齐和精确 byte size，并保持 provenance；
- [x] `TextEmbeddingNode` 只依赖 `IEmbeddingModel`；
- [x] Embedding 的 ONNX Load/Run 只有一份生产实现；
- [x] 两个目标配置的 Embedding 项完成新方言迁移；
- [x] validate、plan、build 和真实 smoke 全部通过；
- [x] 新增测试全部进入 CTest；
- [x] 真实 ONNX 条件测试入口支持环境变量与 fixture；
- [x] 真实 ONNX 条件测试至少一次非 skip PASS，并保存 artifact 证据；
- [x] ONNX ON/OFF 构建均无新增 warning；
- [x] 当前 macOS 环境的 UBSan 覆盖新增边界用例，fast 矩阵 79/79；
- [ ] **合入门禁（不阻塞阶段 4）**：在受支持的 Linux CI 补充 ASan/LSan 证据；当前 macOS 26.6 的
      Apple Clang 16 ASan 在运行时初始化阶段失败，不属于业务测试失败；
- [x] 完整六阶段门禁 100% 通过（82/82 全绿）；
- [x] 本文所有 P0/P1 项关闭并补充验收证据。

阶段 3 实现与本地验收条件已经满足，可以在整改计划中标记完成并继续阶段 4。Linux
ASan/LSan 是最终合入 `main` 前的跨平台质量门禁；在该证据和后续阶段未完成前，RFC-0015
整体状态必须保持 `In Implementation`。
