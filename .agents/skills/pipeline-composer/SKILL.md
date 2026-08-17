---
name: pipeline-composer
description: >-
  Guide for high-reusability pipeline composition (0 C++ lines) and authoring generic, parameterized common nodes.
  Use when creating new algorithmic workflows via JSON configuration, reusing existing operator assets,
  or writing decoupled reusable nodes in src/common_nodes/.
---

# LLM-EdgeFlow: Pipeline Composer & Node Reusability Guide

This skill guides developers and AI agents in **maximizing module reusability** and building new AI pipelines with **zero unnecessary C++ code bloat**.

---

## 🎯 Core Philosophy: Config-First & Composition-First (配置优先，复用优先)

Before writing any new C++ operator class, always ask:
1. *Can I achieve this new business by wiring existing common nodes in a JSON configuration?* (90% of NLP / RAG / Multi-Modal workflows can!)
2. *If a new operator is genuinely required, can it be designed as a generic, parameterized component in `src/common_nodes/` rather than a one-off hardcoded business node?*

```text
[新算法业务需求] 
       │
       ▼
[阶段 1: 零 C++ 代码纯配置组装] ➔ 查阅算子资产库，编写 configs/pipeline_xxx.json
       │ (若需要全新算法算子)
       ▼
[阶段 2: 高内聚通用算子开发] ➔ 遵循 4 大解耦原则，写入 src/common_nodes/，供全团队复用
```

---

## 📦 1. 现有通用算子资产库 (Reusable Node Catalog)

| 算子类名 (`node_type`) | 所在文件 | 核心能力 | 关键可配置参数 (JSON) |
| :--- | :--- | :--- | :--- |
| **`PromptBuilderNode`** | `src/common_nodes/` | 通用提示词组装与变量插值 | `prompt_template` (支持 `{{query}}`, `{{context}}`), `input_key`, `output_key` |
| **`VectorSearchNode`** | `src/common_nodes/` | 通用 Top-K 余弦相似度检索 | `top_k` (默认 3), `similarity_threshold` (默认 0.6) |
| **`DenseRetrievalNode`** | `src/business/dialogue_audit/` | 密集文本向量提取 (Embedding) | `model_id`, `input_key`, `output_key` |
| **`LlmGenerateNode`** | `src/business/doc_qa/` | 硬件定长分批 LLM 生成 | `model_id`, `max_batch_size`, `temperature` |
| **`CrossRerankBatchNode`**| `src/business/cross_rerank/` | 通用 Cross-Encoder 语义精排打分 | `model_id`, `score_threshold`, `top_k` |
| **`KeywordMatcherNode`** | `src/business/keyword_match/`| Aho-Corasick 敏感词/类别规则快筛 | `categories` (支持运行时 `Alg_Control` 动态增删词表) |
| **`SlotExtractNode`** | `src/business/audio_asr/` | 正则与语义槽位抽取 | `slot_rules` |

---

## 🛠️ 2. 通用算子设计 4 大解耦原则 (Decoupling Rules)

当必须编写新算子时，**必须且只能放入 `src/common_nodes/`**，并严格遵守以下 4 条解耦规范：

### 原则 1: 黑板 Key 必须参数化（禁止硬编码字符串）
```cpp
// ❌ 错误示范：硬编码特定业务字段名，无法跨业务复用
auto* text = req_ctx->Get<std::string>("user_medical_record");

// ✅ 正确示范：从 JSON 配置读取字段名，默认提供合理 fallback
input_key_  = config.value("input_key", "raw_text");
output_key_ = config.value("output_key", "processed_text");

auto* text = req_ctx->Get<std::string>(input_key_);
if (!text) return -1;
req_ctx->Set(output_key_, result);
```

### 原则 2: 提示词与业务规则模板化（配置注入）
```cpp
// ✅ 将 Prompt 骨架与占位符置于 JSON，算子只做通用字符串替换
prompt_template_ = config.value("prompt_template", "请处理以下内容：{{input}}");
```

### 原则 3: 模型引擎松耦合（只通过 `model_id` 索取接口）
```cpp
// ✅ 算子不绑定任何具体的底层引擎或芯片，只根据 model_id 索取纯虚接口
model_id_ = config.value("model_id", "default_llm");
auto engine = session_ctx_->GetModelManager().GetModel<ILlmEngine>(model_id_);
```

### 原则 4: 算子无状态化（Stateless Execution）
- 所有针对单个请求的状态与中间数据均存入 `AlgContext`；
- 算子类成员变量仅存放只读配置，确保多线程并发调用时 100% 安全。

---

## 📋 3. 零代码构建新业务工作流示范 (Example)

**需求**：需要新增一个“智能法律条文合规纠错”业务。

**操作步骤（0 行 C++）**：
1. 直接创建 `configs/pipeline_legal_correction.json`：
```json
{
  "business_name": "legal_correction",
  "models": [
    {
      "model_id": "qwen_shared",
      "model_path": "./models/qwen_1.5b.bin",
      "engine_type": "mock_npu_llm"
    }
  ],
  "pipeline": [
    {
      "node_type": "PromptBuilderNode",
      "prompt_template": "你是一名资深法律顾问，请核验并纠错以下法条内容：\n{{query}}",
      "input_key": "raw_legal_text",
      "output_key": "llm_prompts"
    },
    {
      "node_type": "LlmGenerateNode",
      "model_id": "qwen_shared"
    }
  ]
}
```
2. 在 `demo/` 或测试用例中直接调用该配置执行！
