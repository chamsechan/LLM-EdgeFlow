# RFC-0015 阶段 4 Rerank 最终验收结论

- **验收日期**：2026-08-29
- **关联 RFC**：`0015-model-capability-backend-decoupling`
- **关联分支**：`feat/model-backend-decoupling-rfc`
- **最终结论**：**阶段 4 通过，可以进入阶段 5**
- **RFC 状态**：继续保持 `In Implementation`

本文档是阶段 4 的最终验收记录。阶段 4 只完成 Rerank 能力迁移；LLM、OCR、ASR 迁移及旧 Engine 总清理仍属于后续阶段，因此不得提前把 RFC-0015 标记为 `Completed`。

## 1. 验收范围与结果

| 验收项 | 结果 | 证据 |
| :--- | :---: | :--- |
| Model / Backend 解耦 | PASS | `BgeRerankerModel` 仅依赖 `ITensorGraphSession`，不包含 ONNX Runtime API |
| Node 能力依赖 | PASS | `TextRerankNode` 仅绑定 `IRerankModel`，排序、Top-K 和 provenance 仍由 Node 负责 |
| Backend 复用 | PASS | Embedding 与 Rerank 共用 `OnnxRuntimeBackend`；旧 `OnnxRerankEngine` 已删除 |
| Pair tokenizer | PASS | 覆盖 `[CLS] query [SEP] candidate [SEP]`、`token_type_ids`、longest-first 截断与非法 UTF-8 |
| 创建期 metadata | PASS | 输入/输出名称、dtype、rank、动态维、零维、静态 batch、sequence 和输出 shape 均 fail-close |
| 运行期 tensor 防御 | PASS | 覆盖字节数、对齐、空指针、shape、NaN/Inf、批次和 provenance |
| 配置与 Validator | PASS | 4 个受影响配置可 validate/plan；规划不依赖文件是否存在 |
| 可复现测试资产 | PASS | ONNX 和 vocab 由构建期脚本确定性生成，不依赖被忽略的本地模型 |
| Operator / C ABI | PASS | adapter、Operator API、golden 均真实执行 ONNX fixture，无意外 skip |
| Pipeline 端到端 | PASS | 生产配置结构经过 Validator、Build、Execute 并验证真实输出 |
| Demo 行为 | PASS | fixture-backed Demo 成功；缺模型即使启用样例回退仍返回失败，不伪造分数 |
| 架构与变更记录 | PASS | PlantUML/SVG 与当前 Model/Backend 架构一致，README 已记录 RFC-0015 实施进度 |

## 2. 本轮最终整改

### 2.1 Metadata 契约闭包

`BgeRerankerModel::Create` 现在遵循以下规则：

- 负数表示动态维；`0` 不是动态维，输入和输出出现零维时立即拒绝；
- 静态 sequence 必须等于 `model_config.max_length`；
- 固定批次必须与 `fixed_batch_size` 相等；
- 动态批次策略下，静态 metadata batch 仍不得超过 Session 的 `max_batch_size`；
- 输出仅接受 `[B]` 或 `[B, 1]`，第二维为 `0` 或大于 `1` 均拒绝。

对应测试覆盖 20 组创建期正反例，并保留运行期 tensor 边界测试。

### 2.2 Demo 严格 fail-close

`allow_fallback_sample` 只负责在数据集缺失时提供输入样例，不再影响模型加载或推理结果：

```text
数据集缺失 + 显式允许样例回退  -> 可补输入样例
Backend / 模型缺失             -> 返回非零错误
推理执行失败                    -> 返回非零错误
任何真实推理失败                -> 禁止生成伪造分数
```

`cross_rerank_mock` 已从 smoke 套件移到 real 套件，因为它现在使用真实 ONNX Runtime，而不是 Mock Engine。CTest 通过构建目录中的确定性 fixture 单独验证真实 Demo 成功链路，同时验证缺模型链路必须失败。

### 2.3 Fixture 与 CMake 产物收敛

- 生成脚本只产生 CMake 明确声明的 `embedding_fixture.onnx`、`rerank_fixture.onnx` 和 `vocab.txt`；
- Demo fixture 的 Pipeline/`.conf` 以测试模板形式纳入仓库，模型文件仍在构建期生成；
- 测试不会创建或依赖 `models/bge_reranker_large.onnx`、`models/vocab.txt` 等本地隐式资产；
- 不提交大模型、第三方二进制或预编译推理库。

## 3. 最终复验证据

| 验证项 | 结果 | 指标 |
| :--- | :---: | :--- |
| `OnnxAndRerankerModelTest.*` | PASS | 9/9；含真实 ORT 与真实 Pipeline |
| Rerank adapter / Operator / golden | PASS | 3/3；无 skip |
| Demo 定向门禁 | PASS | smoke、fixture success、missing-model fail-close 共 3/3 |
| ONNX ON CTest | PASS | 85/85 |
| ONNX OFF 独立 CTest | PASS | 83/83 |
| 六阶段完整回归 | PASS | 85/85，全部阶段通过 |
| UBSan fast | PASS | 80/80 |
| 架构文档与图形门禁 | PASS | PlantUML 可渲染，SVG provenance 一致 |
| 格式与 diff | PASS | `format.sh --check`、`git diff --check` 均通过 |

确定性 fixture SHA-256：

- Rerank ONNX：`453943c04537a77dbd5b33fc70a37da2b465935496afadd4716329868dfa55be`
- vocab：`e64b6b973844c22250b008ab9b632c04667c3579f75575de64ca4d36c69d81b0`

## 4. 复验命令

```bash
./scripts/format.sh --check
git diff --check

cmake -S . -B build -DENABLE_ONNXRUNTIME=ON
cmake --build build -j4

./build/edgeflow_test_core_runner \
  '--gtest_filter=OnnxAndRerankerModelTest.*'

./build/edgeflow_test_adapter_runner \
  '--gtest_filter=DifferentIoModalitiesTest.CrossRerankBatch:OperatorApiTest.EndToEndCrossRerank:OperatorGoldenTest.CrossRerankGolden'

ctest --test-dir build \
  -R 'CrossRerankDemo(Fixture|MissingModelFailsClosed)Test|DemoSmokeTest' \
  --output-on-failure

ctest --test-dir build --output-on-failure
./scripts/run_all_tests.sh --full
LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh --fast
```

ONNX OFF 必须使用独立构建目录：

```bash
cmake -S . -B build-no-onnx-stage4 \
  -DENABLE_ONNXRUNTIME=OFF \
  -DENABLE_LLAMACPP=OFF
cmake --build build-no-onnx-stage4 -j4
ctest --test-dir build-no-onnx-stage4 --output-on-failure
```

## 5. 阶段 4 完成清单

- [x] Rerank Model 与具体 Backend 解耦；
- [x] TextRerankNode 仅依赖模型能力接口；
- [x] Embedding / Rerank 复用 ONNX Runtime Backend；
- [x] 旧 Rerank Engine 清理完成；
- [x] metadata 与运行期 tensor 契约完整 fail-close；
- [x] fixture、Pipeline、Operator、C ABI、Demo 均可在干净检出复现；
- [x] ONNX ON/OFF、完整回归、UBSan、格式和架构门禁全部通过；
- [x] RFC-0015 保持 `In Implementation`；
- [x] 已具备进入阶段 5 的条件。

阶段 5 应严格以单独的实施文档为准，不在阶段 4 提交中提前实现 LLM 迁移。
