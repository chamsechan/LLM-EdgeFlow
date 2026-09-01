# RFC-0025 部署运行时契约收敛验收报告

- 验收日期：2026-09-01
- 基线：`origin/main@e6944ec4450fb3d0cecf18e1d6675b2d4f715700`
- 候选分支：`refactor/deployment-runtime-contract-convergence`
- 机器证据：`build/acceptance/rfc-0025.json`（由
  `scripts/generate_acceptance_evidence.sh` 生成，包含基线 SHA、HEAD SHA、完整源文件内容
  SHA-256、工作树状态和门禁结果；artifact 哈希由 pinned 下载脚本实际计算）

## 1. 验收结论

RFC-0025 范围内的 3 个 P0、4 个 P1 和 3 个 P2 问题均已关闭。实现未新增 Node、
Model capability 或 vendor Backend；受影响 Pipeline 均继续使用可执行 Catalog 中已有
Definition。真实执行结论只覆盖 CPU ONNX Runtime 与 CPU llama.cpp，不代表 AX650 或
其他硬件验收。

## 2. 问题闭环矩阵

| 优先级 | 问题 | 最终结论与证据 |
| --- | --- | --- |
| P0 | `model_root_dir` 语义、拼接和三入口一致性 | C ABI root 冻结为直接包含 artifact/sidecar 的目录；Operator 保持 bundle root 契约。Layer 1 按各入口语义生成沙箱内绝对模型路径，官方 C ABI JSON 不再含重复 `models/`。 |
| P0 | `device_id` 是否进入生产 Backend | `ExecutionTarget` 已贯通 Session → ModelLoadSpec → BackendLoadSpec；mock capture、C ABI ONNX 非零设备拒绝、ONNX/llama.cpp 生产诊断均通过。 |
| P0 | real Demo/Profile 可复现 | pinned fetch 脚本校验 5 个 artifact/sidecar SHA-256；`cross_rerank_onnx` 与 `doc_qa_onnx` 实际运行成功。 |
| P1 | 部署职责进入 Layer 2 | Validator 已删除 root 参数；Session 已删除 config/root 部署状态，Layer 2 只做词法路径校验。 |
| P1 | 默认 CI 真实 C ABI、real profile、sanitizer | CI 新增完整生产 Backend ASan/UBSan、真实 GGUF C ABI + public Profile 和 exact-SHA evidence jobs。 |
| P1 | Session/ModelManager 冗余状态 | ModelManager 删除 model/revision 镜像 map，只保留单一 `ModelRegistration` 状态源；生产 revision 消费者保持通过。 |
| P1 | ONNX 非 Tensor I/O fail closed | 在 tensor API 前显式检查 ONNX 类型；sequence-output fixture 返回固定诊断。 |
| P2 | 动态库版本、RFC、Changelog、ABI | 产品/共享库 VERSION 为 8.0.0，SONAME/C ABI major 为 5；未变化的内部 Biz Adapter descriptor 保持 2.0.0，RFC 索引和 Changelog 同步。 |
| P2 | Demo mock/real/chip 标识 | mock profile 明确命名并声明 CPU emulator；真实 ONNX profile 使用 `cpu_generic`，原 `cross_rerank_mock` 更名；无 Profile 的 Demo 默认平台改为 CPU。 |
| P2 | 验收证据绑定最新 main SHA | 2026-09-01 重新 fetch 并确认本地/远端 main 同为上述 SHA；本报告与机器证据替代旧综合报告作为本 RFC 验收依据。 |

## 3. 验收命令

| 门禁 | 结果 |
| --- | --- |
| 全部生产 Pipeline `alg_pipeline_tool validate` + `plan` | PASS |
| `./scripts/fetch_real_test_models.sh --all` | PASS，5/5 SHA-256 匹配 |
| `alg_demo --suite real` | PASS，4/4 CPU real Profiles，覆盖 ONNX Runtime、tokenizer sidecar 与 llama.cpp |
| `./scripts/run_real_model_e2e.sh` | PASS，3/3 real tests，无 skip；随后 public real Profile PASS |
| `./scripts/run_sanitizers.sh --full` | PASS，85/85，生产 ONNX Runtime 与 llama.cpp 均启用 |
| `./scripts/run_all_tests.sh` | PASS，85/85 CTest |

## 4. 发布边界

版本号在代码和文档中作为未正式发布的 v8 里程碑生效；本次工作不创建 Git tag、Release、
PR 或远端合并。真实权重和 sidecar 保持 Git 忽略，只由固定提交 URL 与 SHA-256 脚本获取。
