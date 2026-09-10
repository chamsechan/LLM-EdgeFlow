# 示例方案配置

文件统一采用 `pipeline_<方案>_<变体>.json`，同名 `.conf` 保存部署配置。
方案文件保持平铺，Pipeline Studio 可直接发现、打开并另存；测试替身方案继续位于
`demo/fixtures/mock/`。Model、Backend、权重和节点参数以文件内容与原生 Catalog 为准。

`default` 保留原始方案参数；`cpu` 是开源 Backend 的 CPU 演示配置（ASR 需启用 whisper.cpp）；`kite`
使用可选 Kite Backend（部分方案同时使用 ONNX Runtime）；`rules` 只运行规则节点。
这些名字不表示效果或生产验收已经通过。

需要选择可运行预设时查询 `alg_pipeline_tool catalog` 的 `profiles` 并核对资源；详细流程见
[Pipeline Studio](../tools/pipeline_studio/README.md)。

## 路径迁移（RFC-0046）

下表中的路径同时适用于 `.json` 和 `.conf`，只改变文件位置和引用，保留方案内容。

| 原路径（省略扩展名） | 当前路径（省略扩展名） |
| --- | --- |
| `configs/pipeline_doc_qa` | `configs/pipeline_doc_qa_default` |
| `configs/pipeline_doc_qa_onnx` | `configs/pipeline_doc_qa_cpu` |
| `configs/pipeline_doc_qa_rerank` | `configs/pipeline_doc_qa_rerank_default` |
| `configs/pipeline_doc_qa_rerank_real` | `configs/pipeline_doc_qa_rerank_cpu` |
| `configs/pipeline_entity_extract` | `configs/pipeline_entity_extract_default` |
| `configs/pipeline_entity_extract_llamacpp` | `configs/pipeline_entity_extract_cpu` |
| `configs/pipeline_dialogue_audit` | `configs/pipeline_dialogue_audit_default` |
| `configs/pipeline_audio_asr_whisper` | `configs/pipeline_audio_asr_cpu` |
| `configs/pipeline_cross_rerank` | `configs/pipeline_cross_rerank_cpu` |
| `configs/pipeline_keyword_match` | `configs/pipeline_keyword_match_rules` |
| `configs/kite/pipeline_entity_extract` | `configs/pipeline_entity_extract_kite` |
| `configs/kite/pipeline_doc_qa_rerank` | `configs/pipeline_doc_qa_rerank_kite` |
| `configs/kite/pipeline_doc_qa` | `configs/pipeline_doc_qa_kite` |
| `configs/kite/pipeline_ocr_doc_qa` | `configs/pipeline_ocr_doc_qa_kite` |
| `configs/kite/pipeline_dialogue_audit` | `configs/pipeline_dialogue_audit_kite` |
| `configs/kite/pipeline_doc_qa_embeddings` | `configs/pipeline_doc_qa_kite_generated_embeddings` |

## Profile 名称迁移

以下旧名称由新名称替代；`--biz` 与 Pipeline `biz_name` 保持原样。

| 原 Profile | 当前 Profile |
| --- | --- |
| `keyword_match_mock` | `keyword_match_rules` |
| `entity_extract_llamacpp` | `entity_extract_cpu` |
| `doc_qa_onnx` | `doc_qa_cpu` |
| `doc_qa_rerank_real` | `doc_qa_rerank_cpu` |
| `cross_rerank_onnx` | `cross_rerank_cpu` |
| `audio_asr_whisper` | `audio_asr_cpu` |
