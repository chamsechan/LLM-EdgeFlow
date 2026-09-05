# RFC 0035: Kite 生成 token 向量与中性 Embedding 接入

- **RFC 编号**：0035-generated-token-embedding
- **创建日期**：2026-09-05
- **文档状态**：Completed
- **关联分支**：`feat/kite-capability-coverage`
- **目标版本**：v10.x
- **负责人 / 作者**：LLM-EdgeFlow contributors

## 1. 背景与动机

RFC-0034 覆盖了文本和图像生成，但遗漏 Kite 的原生向量输出。
锁定的 kiteLLM v0.1.0 / `5e58820f39919fce2046e2fd703c62601a5df59b`
提供 `SetVerboseDataFlag`、`GetOutputEmbedding`，并在
`src/engine/generator.cpp` 中先 decode 生成 token，再读取该 token 的隐藏向量。
因此它不是 BGE encoder 的输入 token 矩阵，也不是 prefill 最后一个 token 的向量。

## 2. 范围与边界

新增生成 token 向量执行协议、Kite 实现、复用 IEmbeddingModel 的语义模型，
以及显式选择该模型的纯 Kite doc_qa 配置。既有 BGE/ONNX 路径继续可用。
不声称生成模型已经达到专用检索模型的准确率，不接管 ONNX BGE，不修改 SDK，
不将输出向量缺失降级为伪造向量。ASR、流式公共 ABI 和业务无关的原生调试 API 不在本次范围。

## 3. 架构与数据流

仅新增 Layer 4 能力；Layer 1~3 接口不变：

`TextEmbeddingNode → IEmbeddingModel (generated_text_embedding)`
`→ IGeneratedTokenEmbeddingSession → Kite C API`

中性协议接受已经格式化的 prompt、BOS 策略和生成上限，返回拥有自身内存的
token IDs 与逐 token float 向量。约定 greedy 生成、最多 64 个 token，EOS 可提前结束。
Backend 不做池化、归一化或业务 prompt 拼接。协议枚举追加，不改变原有值。
Model 负责 prefix/suffix、last/mean 池化、维度校验、归一化；使用
FixedBatchExecutor 保持 req_id/sub_id 和失败全量回滚。模型维度显式声明，
禁止截断或补齐向量以伪装兼容旧索引。生产索引更换模型时必须重建并评估。

## 4. 权衡与不变量

- SDK 输出在 task 销毁前复制；句柄、运行时 RAII 和 session 串行锁保持一致。
- 生成与向量会话共享 Backend 内的 token 输入、采样设置与任务执行设施。
- 校验输出行数、token 数、指针、有限数值和维度；空输出/EOS 首 token 即结束明确失败。
- 单请求实际生成量可小于上限；mean/last 只使用实际返回的向量。
- 模型配置可见于 Catalog；无 vendor include 向 Model/Node 泄漏。
- 生成文本向量作为显式可选能力，效果与开销须按模型、prompt 和数据集验证。

## 5. 验收计划

- 扩展 ModelBackendDecouplingTest：协议错误、配置边界、池化与归一化、维度/NaN/
  空输出、批次溯源与失败回滚。
- 扩展 LlamaCppBackendTest：真实 Kite 向量形状、有限数值、重复请求及并发一致性，
  新协议的注册与原生 API compile/link 检查。
- Catalog/validate/plan 检查新配置，真实 doc_qa Demo 链路验证。
- canonical `./scripts/run_all_tests.sh` 及非默认 Kite 真实模型 gate。

## 6. 实施进度

实现及本地验收完成；本地交付，不上传。2026-09-05 Linux aarch64 CPU 证据：

- canonical `LLM_EDGEFLOW_JOBS=8 ./scripts/run_all_tests.sh`：88/88，包含格式、
  分层、默认 ONNX/llama.cpp 和全部 CTest；日志 `/tmp/kite-embedding-default-gate.log`。
- Kite 构建开启真实 text/vision/demo 环境变量后完整 CTest：88/88；
  日志 `/tmp/kite-embedding-real-gate.log`。真实生成向量测试实际执行，验证非空有限向量
  和并发重复调用一致性。默认构建中该真实 SDK 用例按既有约定跳过。
- 六份 Kite pipeline 的 validate/plan 共 12 项成功，产物
  `results/kite-embedding-validation/`。
- `doc_qa_kite_embeddings` CLI 两条样本全部 status=0；8 个部署 Profile 共 13 条样本
  全部成功，结果在 `results/kite-deployment-tests/`。其中既有混合配置仍含 ONNX，
  新增问答配置的向量与文本生成均使用 Kite。
- 新问答样本返回了文本，但有生成上限截断和超出参考片段的内容。本次只验收功能、
  数值和契约，不作为召回率、回答准确率或性能验收；生产部署仍需目标数据评估。
- 未执行远程 CI、x64、GPU/NPU 或 sanitizer；CI 定义已纳入新协议与配置检查。

## 7. 变更记录

| 日期 | 变更 |
| :--- | :--- |
| 2026-09-05 | 按原生源码明确生成 token 向量语义，补充 RFC-0034 遗漏能力 |
