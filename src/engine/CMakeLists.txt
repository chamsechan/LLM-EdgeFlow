target_sources(edgeflow_layer4_engine_objects PRIVATE
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
  models/qwen_causal_lm/qwen_causal_lm_model.cpp
  models/generated_text_embedding/generated_text_embedding_model.cpp
  models/vision_document/vision_document_model.cpp
  models/vision_document/image_decode.cpp)

target_link_libraries(edgeflow_layer4_engine_objects PRIVATE edgeflow_stb_image)
