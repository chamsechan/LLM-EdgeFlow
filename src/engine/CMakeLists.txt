target_sources(alg_sdk PRIVATE
  runtime/backend_registry.cpp
  runtime/model_registry.cpp
  runtime/model_runtime_factory.cpp
  text/utf8.cpp
  text_generation/common_autoregressive_generator.cpp
  backends/onnxruntime/onnxruntime_backend.cpp
  backends/llama_cpp/llama_cpp_backend.cpp
  backends/kite_llm/kite_llm_backend.cpp
  models/bge_common/bert_model_support.cpp
  models/bge_embedding/bert_wordpiece_tokenizer.cpp
  models/bge_embedding/bge_embedding_model.cpp
  models/bge_reranker/bge_reranker_model.cpp
  models/qwen_causal_lm/qwen_causal_lm_model.cpp)
