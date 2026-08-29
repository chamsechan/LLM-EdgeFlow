# RFC-0015 阶段 6：OCR、ASR 与测试替身验收结论

- **验收日期**：2026-08-29
- **适用分支**：`feat/model-backend-decoupling-rfc`
- **实施依据**：[../0015-model-capability-backend-decoupling.md](../0015-model-capability-backend-decoupling.md)
- **阶段结论**：**PASS，可以进入阶段 7**
- **RFC 状态**：继续保持 `In Implementation`

## 1. 结论

阶段 6 已完成 OCR、ASR Node 从组合 Engine DTO 到 typed Model capability 的迁移，并把
五类带业务固定响应的 Mock NPU 实现整体移出生产源码和 `alg_sdk`。

最终 Node 调用链为：

```text
OcrDetectNode -> IOcrModel::Recognize(ImageRefBatch, OcrDocumentBatch)
AsrTranscribeNode -> IAsrModel::Transcribe(AudioPcmBatch, TextBatch)
```

验收确认：

- OCR Node 不再引用 `IOcrEngine::OcrBoxItem`，Model 产出的 `OcrDocumentBatch` 原样写入
  `document` 端口；
- ASR Node 不再构造 `IAudioAsrEngine::AudioPcmData`，`AudioPcmBatch` 以 const reference
  直接传给 Model；
- 两个 Node 均校验输出数量和 `(req_id, sub_id)` provenance，异常结果 fail-close；
- typed fake Model 位于 `tests/support/inference/`，不注册到生产 ModelRegistry；
- 旧 Mock NPU 源码已从 `src/engine/mock_npu/` 移到测试支持目录，仅由测试与 Demo
  smoke 目标显式编译；
- 独立生产 Catalog 不包含 `mock_npu_*`、`test_tensor_backend` 或
  `test_causal_lm_backend`；
- 现有 OCR/ASR C ABI、Node Type、端口、Blackboard Key 和业务 DAG 保持不变。

## 2. 关于 PPOCR/Paraformer 注册的验收决定

仓库当前没有可验收的 PPOCR 或 Paraformer 模型文件、图像预处理/双子图编排实现、音频
特征提取/词表解码实现，也没有对应真实 vendor Backend。按照 RFC 第 10.6 节的严格注册
条件，本阶段**不在生产 ModelRegistry 注册空壳 `ppocr` 或 `paraformer_asr`**。

当前采取的正确边界是：

- 生产侧保留 `IOcrModel`、`IAsrModel` 与中性共享 DTO；
- Node 只依赖 typed capability；
- 测试侧以受控 typed fake Model 验证端口、批处理、provenance 和错误回滚；
- 将来接入真实模型时，必须补齐预处理、sidecar、Tensor decode、真实 artifact 与条件测试
  后才能注册具体 Model type。

这避免生产 Catalog 声称支持实际不存在的 OCR/ASR runtime 组合。

## 3. 生产与测试隔离

### 3.1 生产目标

`FRAMEWORK_SRCS` 已删除全部：

```text
src/engine/mock_npu/mock_npu_embedding_engine.*
src/engine/mock_npu/mock_npu_llm_engine.*
src/engine/mock_npu/mock_npu_rerank_engine.*
src/engine/mock_npu/mock_npu_ocr_engine.*
src/engine/mock_npu/mock_npu_asr_engine.*
```

生产 `alg_sdk` 只保留真实 Backend/Model 注册。Catalog 独立进程测试确认：

```text
mock_npu_*             absent
test_tensor_backend    absent
test_causal_lm_backend absent
```

### 3.2 测试与 Demo 过渡 fixture

在阶段 7 删除 legacy 配置方言前，旧回归配置仍需经过 `EngineFactory` 物化。对应 fixture
暂存于：

```text
tests/support/inference/legacy_mock_npu/
```

其中 OCR/ASR fixture 同时实现 legacy 装载接口和 typed Model capability，使旧配置只在
测试/Demo 目标中可执行；该双接口不会进入生产动态库。业务固定回答也因此完全离开生产
源码和 Catalog。

阶段 7 删除 legacy 方言、`EngineFactory` 与旧配置后，应同步删除该过渡目录和 CMake
fixture 源码清单。

## 4. 测试证据

### 4.1 定向测试

| 门禁 | 结果 |
| :--- | :--- |
| `AsrTranscribeNodeTest` | 4/4 PASS |
| `OcrDetectNodeTest` | 3/3 PASS |
| OCR/ASR Common Node 回归 | PASS |
| 错误数量与 provenance 篡改 fail-close | PASS |
| `CatalogContractSsotTest` | 7/7 PASS |
| `LayerGuardTest` | PASS |

### 4.2 全量回归

```text
CTest：86/86 PASS
格式化：PASS
git diff --check：PASS
```

全量 CTest 覆盖 OCR/ASR C ABI、多输入模态、并发边界、Operator API/Golden、Demo smoke、
Pipeline Studio、CLI、架构文档与静态分层门禁。

## 5. 阶段 6 完成清单

- [x] OCR/ASR 使用 neutral payload 与 typed Model capability；
- [x] OCR Node 删除 Engine 嵌套 DTO 转换；
- [x] ASR Node 删除 PCM 深拷贝；
- [x] Node 输出数量和 provenance 严格校验；
- [x] tests/support 提供中性 typed fake Model 与 fake Backend/session；
- [x] 业务固定响应移出生产源码；
- [x] 五类生产 Mock Engine 注册全部移除；
- [x] 生产 Catalog 无 test/mock Backend；
- [x] 无真实实现的 PPOCR/Paraformer 不做空注册；
- [x] 定向测试、全量 CTest、格式与 LayerGuard 全部通过。

## 6. 阶段 7 必须完成的收口项

阶段 7 不能保留本阶段的过渡 fixture 或双轨配置，必须完成：

1. 删除 `IModelEngine`、`EngineFactory`、`EngineDefinition`、legacy adapters 和旧注册表；
2. 删除 `engine_type`、旧 `config`、`ModelConfigDialect` 及 legacy parser/build 分支；
3. 逐份迁移或移出仍依赖 `mock_npu_*` 的生产配置；
4. 删除 `tests/support/inference/legacy_mock_npu/`；
5. 更新 CLI、Studio、README、architecture、RFC 与索引；
6. 完成 runtime ON/OFF、CTest、`run_all_tests.sh --full`、sanitizer、Demo 和
   validate/plan 全矩阵。

阶段 7 和最终全量门禁尚未完成，因此当前不得把 RFC 标记为 `Completed`，也不得合入
`main`。
