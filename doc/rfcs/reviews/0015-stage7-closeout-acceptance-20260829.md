# RFC-0015 阶段 7 收口与最终验收结论

- **验收日期**：2026-08-29
- **适用分支**：`feat/model-backend-decoupling-rfc`
- **实施依据**：[../0015-model-capability-backend-decoupling.md](../0015-model-capability-backend-decoupling.md)
- **阶段结论**：**PASS，RFC-0015 实施闭环，可进入 PR/CI 合入门禁**
- **本地验收范围**：源码、配置、Catalog/CLI/Studio、Demo、文档、Release 全量与 UBSan

## 1. 结论

阶段 7 已删除过渡期的组合 Engine API、旧配置方言和双路物化逻辑。
当前唯一推理调用链为：

```text
Node
  -> typed Model capability
  -> concrete Model semantics
  -> neutral TensorGraph / CausalLm protocol
  -> concrete Backend Session
  -> vendor runtime
```

生产 `alg_sdk` 只注册真实实现：

```text
Models:   bge_embedding, bge_reranker, qwen_causal_lm
Backends: onnxruntime, llama_cpp
```

OCR/ASR 的 typed capability 和 Node 契约已完成；因仓库未交付真实
PPOCR/Paraformer artifact 及预后处理实现，确定性模型只存在于
`tests/support/inference/` 且仅链接测试与 Smoke Demo，不进入生产
Catalog。

## 2. 阶段 7 收口项

- [x] 删除 `include/engine/engine_interface.h` 和
  `include/engine/engine_registry.h`；
- [x] 删除 `IModelEngine`、`EngineFactory`、`EngineDefinition`、旧 ONNX
  Embedding Engine 和全部 Legacy MockNPU Engine；
- [x] `ModelManager` 只保存 typed `IModel`，Pipeline 只经
  `ModelRuntimeFactory` 物化并通过 `RegisterBatch` 原子提交；
- [x] Parser 只允许 `model_id/capability/model_type/backend/model_path/
  model_config/backend_config/comment`，旧字段以 `UNKNOWN_FIELD` 拒绝；
- [x] Validator、Catalog、`alg_show`、`alg_pipeline_tool` 和 Studio 只使用
  ModelRegistry/BackendRegistry SSOT；
- [x] 生产配置已迁移至 Model/Backend 语法，不存在
  `engine_type`；
- [x] OCR/ASR 无真实实现的产品配置从 `configs/` 移出，对应
  Smoke 配置收敛到 `tests/fixtures/stage7/`；
- [x] README、architecture、developer guide、PlantUML 和 SVG 已与最终架构同步。

## 3. RFC 验收标准矩阵

| # | 验收项 | 结果 | 证据 |
| :---: | :--- | :---: | :--- |
| 1 | Node 只依赖五个 typed Model capability | PASS | 五个 common inference Node 只 include `model_interface.h` |
| 2 | Model 不 include 具体 Backend/vendor | PASS | `src/engine/models/` 静态扫描无 vendor 头 |
| 3 | IModel 不暴露 Backend，Model 不按 BackendType 分支 | PASS | `model_interface.h` 与 Model 源码扫描 |
| 4 | Backend 不依赖 Node/Pipeline/AlgContext/业务逻辑 | PASS | LayerGuard 与 Backend 源码扫描 |
| 5 | vendor 头只在对应 Backend | PASS | `llama.h`/`onnxruntime_cxx_api.h` 各只有一处 |
| 6 | 无 Model×Backend 组合注册 | PASS | 仅 `REGISTER_MODEL_WITH_DEFINITION` / `REGISTER_BACKEND_WITH_DEFINITION` |
| 7 | ONNX 加载、metadata、binding、Run 单一实现 | PASS | 只有 `OnnxRuntimeBackend` |
| 8 | Embedding/Rerank 复用 ONNX Backend | PASS | 两个 Model 均只消费 `ITensorGraphSession` |
| 9 | llama Backend 不含 ChatML/sampling/业务回答 | PASS | LayerGuard LLM 语义边界检查 |
| 10 | Qwen Model 不持有 llama handle | PASS | `llama.h` 仅在 Backend |
| 11 | OCR/ASR Node 不复制 Engine 嵌套 DTO | PASS | 直接传递 neutral payload |
| 12 | 批推理只用 `FixedBatchExecutor` | PASS | 五能力生产/测试路径及 BatchExecutorTest |
| 13 | Core 不读 platform/chip/device/provider | PASS | Core 代码与 config schema 扫描 |
| 14 | backend_config 只由 Definition 校验、Backend 解释 | PASS | Validator/RuntimeFactory 传递一致性测试 |
| 15 | 切换 Backend 不改 Node/port/DAG | PASS | ValidatedPipelinePlan 与配置回归 |
| 16 | 协议不兼容在 Load 前 fail-close | PASS | ModelBackendPipelineTest |
| 17 | 物化失败无部分注册 | PASS | RegisterBatch 回滚/生命周期测试 |
| 18 | 生产 Catalog 无 test/mock | PASS | Catalog 独立进程输出只含 3 Model + 2 Backend |
| 19 | 生产 Backend 无 emulator/fallback | PASS | 缺文件/缺 runtime 测试均 fail-close |
| 20 | 生产配置无 `engine_type` | PASS | `configs/` 扫描为零 |
| 21 | CTest/全量脚本/Demo/validate+plan | PASS | 见第 4 节 |
| 22 | README/architecture/Catalog/Studio/RFC 一致 | PASS | Docs drift + PlantUML render/check |

## 4. 测试证据

| 门禁 | 结果 |
| :--- | :--- |
| `./scripts/format.sh --check` + `git diff --check` | PASS |
| Release CTest | **84/84 PASS** |
| `./scripts/run_all_tests.sh --full` | **84/84 PASS，6/6 门禁 PASS** |
| 干净重链接后的 Visualizer/CLI 诊断矩阵 | **6/6 PASS** |
| `LLM_EDGEFLOW_SANITIZERS=undefined ./scripts/run_sanitizers.sh --fast` | **80/80 PASS** |
| `./build/alg_demo --suite smoke` | **7/7 profiles PASS** |
| 生产 Pipeline `validate + plan` | **9/9 PASS** |
| Stage 7 fixture Parse/Validate/Build + Demo | **6/6 PASS** |
| Stage 7 fixture Native CLI 结构展示 | **OCR/ASR 2/2 PASS** |
| 架构文档与 SVG render/check | PASS |
| 生产 Legacy API/config/test-backend 扫描 | 0 命中 |

### 4.1 ASan 本机限制

`./scripts/run_sanitizers.sh --fast` 的 ASan+UBSan 构建成功，但在当前
macOS/AppleClang 环境中，所有二进制在进入 `main()` 前均由 ASan runtime
以下断言终止：

```text
AddressSanitizer: CHECK failed: sanitizer_malloc_mac.inc:189
"((!asan_init_is_running)) != (0)"
```

独立的空 `main()` 最小探针使用同一
`-fsanitize=address,undefined` 同样复现，证明该结果为工具链/操作系统
限制，而非项目测试触发的内存问题。本地不将 ASan 记为 PASS；
Linux CI 仍应执行 ASan/LSan，本地 UBSan 已完整通过。

## 5. 生产与 fixture 边界

`alg_pipeline_tool` 属于生产目标，对 `test_business_*` /
`test_*_backend` 返回 unknown 是预期的隔离行为。Stage 7 fixture 的
validate/plan/build 由显式链接测试注册项的 GTest 目标、`alg_demo` 和
仅供诊断矩阵使用的 `alg_pipeline_tool_test` 完成；测试 CLI 与生产 CLI
共用同一入口源码，但只有测试 CLI 链接测试注册项。不得为了让生产 CLI
识别 fixture 而把测试 Model/Backend 链入 `alg_sdk`。

## 6. 合入结论

阶段 1–7 的实现、修复和本地门禁已闭环。当前分支允许进入
标准化 PR 流程；合入前必须保持远端 CI 全绿，并由 Linux CI 承担
本机无法执行的 ASan/LSan 运行时验证。
